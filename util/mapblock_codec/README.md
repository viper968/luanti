# Mapblock codec tools

Tools for the mapblock codec used by on-disk serialization version 30
(`src/mapblock_codec.cpp`). Version 30 has the same layout as version 29 but
replaces its zstd layer with a codec that models the 3D structure of a block.
It is used for disk storage only; network transfer stays at version 29.

| File            | Purpose |
|-----------------|---------|
| `train.cpp`     | Trains the model priors (`src/mapblock_codec_priors.h`) and benchmarks size/speed against zstd |
| `memory.cpp`    | Compares heap usage of the codec with zstd as Luanti uses it |
| `make_world.sh` | Generates a test world (emerges a fixed area, then shuts down) |
| `common.h`      | Parser for uncompressed version 29 blocks (used by `train.cpp`) |

## Results

Minetest Game worlds, 640×192×640 nodes each. Priors were trained on MTG v7 +
carpathian and devtest v7 + carpathian; results are measured on MTG valleys,
v6 and v5 (112,659 blocks the training never saw). Single thread, `-O2`.

| Codec                             | Size      | vs. default | Compress   | Decompress |
|-----------------------------------|-----------|-------------|------------|------------|
| zstd (Luanti default, -1)         | 24.75 MB  | 100%        | 142 µs/blk | 32 µs/blk  |
| zstd (Luanti level 9)             | 20.86 MB  | 84.3%       | 79–109 µs  | 29–32 µs   |
| zstd -19 (not selectable)         | 18.94 MB  | 76.5%       | 1549 µs    | 33 µs      |
| **mapblock codec (version 30)**   | **6.63 MB** | **26.8%** | **48 µs**  | **54 µs**  |

Converting the valleys world in place with `luantiserver --recompress`:
5.39 MB → 1.02 MB of block data, every block decodes to identical content.

On the simpler devtest game the codec reaches 15% of the default size.

### Memory

`mbmemory` on the same 112,659 blocks. *Retained* is what stays allocated
after the run (per-thread contexts, caches, priors); *peak* is the highest
heap use above the starting point at any moment.

| Operation                  | Retained   | Peak       |
|----------------------------|------------|------------|
| compress zstd (Luanti -1)  | 3581 KiB   | 3587 KiB   |
| compress zstd (Luanti 9)   | 25341 KiB  | 25344 KiB  |
| compress mapblock codec    | 461 KiB    | 659 KiB    |
| decompress zstd            | 2530 KiB   | 2580 KiB   |
| decompress mapblock codec  | 461 KiB    | 574 KiB    |

Of the codec's 461 KiB, 367 KiB are the priors, shared by the whole process.
Each thread additionally keeps a 100 KiB model buffer. The extra transient
memory comes from the parsed block (~33 KiB) and zstd for non-empty
metadata/object tails. Stack use is ~20 KiB, similar to Luanti's zstd wrappers
(16–32 KiB of buffers). zstd remains in use for the network and for reading
version 29 blocks.

## How the codec works

zstd sees a mapblock as a flat byte string. The codec models the 3D structure
directly and codes every decision with a binary arithmetic coder (16-bit
probabilities, count-based adaptive learning rate, integer-only so it is
bit-exact on every platform).

1. **Whole-node prediction.** Nodes are visited z+, y- (top-down), x+. The
   distinct values among the neighbours left (x-1), behind (z-1) and above
   (y+1) are candidates, and a cascade of binary decisions asks "is this node
   identical (content + param1 + param2) to candidate *k*?". Context: rank,
   which neighbours share the value, whether the diagonal neighbour agrees,
   whether the previous node was a hit, the class of the candidate
   (air / ignore / other). Most nodes cost a small fraction of a bit.
2. **Per-field fallback.** On a miss, content, param1 and param2 are coded
   separately with the same candidate idea. param1/param2 contexts include
   which neighbours have the same content as this node, and param1 gets an
   extra candidate from a **light propagation predictor** (max neighbour light
   − 1, or 15 directly under sunlight). Escapes use binary trees.
3. **Uniform blocks** (about 2/3 of a typical map) cost a few bits plus their
   name.
4. **Header and names** use static trained probabilities: lighting_complete
   0xFFFF as one bit, timestamp as Elias-gamma, palette sorted by name and
   front-coded, built-in tokens for `air`/`ignore`/`unknown`, an order-1
   character model for the rest.
5. **Trained priors.** Adaptive models start every block from probabilities
   measured on training worlds.
6. **Tail** (node metadata, static objects, node timers) is one bit when
   empty, otherwise appended length-prefixed after the arithmetic stream
   (zstd-compressed when smaller).

The decoder bounds every count, length and id it reads and stops when it reads
more than 16 bytes past its input, so corrupt data only raises
`SerializationError`. This was fuzzed with 240,000 mutated real blocks under
ASan/UBSan.

## Usage

Dependencies: zstd and sqlite3 development headers. Run from the repository
root.

```sh
# test worlds (needs a built luantiserver; any game works)
util/mapblock_codec/make_world.sh bin/luantiserver /tmp/w_v7 minetest_game v7 12345
util/mapblock_codec/make_world.sh bin/luantiserver /tmp/w_v6 minetest_game v6 999

# benchmark the priors compiled into the engine ("-" = no training)
g++ -O2 -std=c++17 -DMAPBLOCK_CODEC_TRAINING -Isrc util/mapblock_codec/train.cpp \
    src/mapblock_codec.cpp -o mbtrain -lzstd -lsqlite3
./mbtrain - /tmp/w_v6/map.sqlite

# memory comparison (Linux/glibc only)
g++ -O2 -std=c++17 -Isrc util/mapblock_codec/memory.cpp src/mapblock_codec.cpp \
    -o mbmemory -lzstd -lsqlite3
./mbmemory /tmp/w_v6/map.sqlite
```

`train.cpp` only reads version 29 worlds (generate them with an engine build
from before version 30, or use existing worlds). `NOZSTD=1` skips the zstd
baselines.

### Retraining the priors

```sh
./mbtrain train1.sqlite,train2.sqlite test.sqlite src/mapblock_codec_priors.h
```

**The priors are part of the version 30 format.** Retraining them, or changing
anything about the models, makes existing version 30 worlds unreadable unless
it comes with a new serialization version. `TestMapBlock::testLoad30` guards
against accidental changes.

## Possible improvements

- Node names are now the second largest cost. A world-wide name table would
  remove most of it, but blocks would no longer be self-contained.
- The order-1 character model is 256 KiB of the priors and mostly unused;
  a smaller model would shrink the shared memory.
- No checksum: like zstd as Luanti uses it, some corruptions decode to a
  wrong but valid-looking block.
