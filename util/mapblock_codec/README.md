# Mapblock codec tools

Tools for the mapblock codec used by on-disk serialization version 30
(`src/mapblock_codec.cpp`). Version 30 has the same layout as version 29 but
replaces its zstd layer with a codec that models the 3D structure of a block.
It is used for disk storage only; network transfer stays at version 29.

| File            | Purpose |
|-----------------|---------|
| `train.cpp`     | Trains the model priors (`src/mapblock_codec_priors.h`) and benchmarks size/speed against zstd |
| `memory.cpp`    | Compares heap usage of the codec with zstd as Luanti uses it |
| `scan.cpp`      | Streams a whole (large) world through zstd and the codec with several threads and verifies every block |
| `make_world.sh` | Generates a test world (emerges a fixed area, then shuts down) |
| `common.h`      | Parser for uncompressed version 29 blocks (used by `train.cpp`) |

## Results

All timings are single-threaded per block, with zstd driven exactly like
`src/serialization.cpp` does (streaming API, per-thread contexts).

### NodeCore server (real, player-built world)

The public world of a long-running NodeCore server (2000³ nodes), 9,003,189
version 29 blocks, measured with `mbscan`. NodeCore was not part of the
training data.

| Codec                           | Block data   | vs. default | Compress | Decompress |
|---------------------------------|--------------|-------------|----------|------------|
| as stored on the server         | 1,043.8 MB   | 99.4%       |          |            |
| zstd (Luanti default, -1)       | 1,050.3 MB   | 100%        | 7.4 µs   | 7.6 µs     |
| zstd (Luanti level 9)           | 925.0 MB     | 88.1%       | 35.6 µs  | 7.6 µs     |
| **mapblock codec (version 30)** | **338.7 MB** | **32.2%**   | 33.8 µs  | 16.1 µs    |

Converting the whole map (9,420,151 blocks, including 416,962 old version 28
blocks) with `luantiserver --recompress` took 8.5 minutes with a peak RSS of
110 MiB. The SQLite file shrinks from 1,531 MB to 680 MB (both vacuumed); the
rest of the file is SQLite's per-row overhead, which this world's old
`pos INT PRIMARY KEY` schema makes large.

Every converted block decodes, and every block was compared with the
original: 8,053,036 are identical. The other 950,153 differ only by what any
re-save by the current engine does, independent of the codec:

- 929,310: the `0x02` flag is recomputed (older servers wrote
  day-night-differs, the engine now writes "not all air").
- 20,945: node metadata fields are written in a different order (same keys
  and values).
- 458: node timer timeouts/elapsed times or static object positions changed
  by one unit (1 ms or 1/1000 node), float rounding when they are loaded and
  saved.

NodeCore ALPHA with its mods then loaded, modified and re-saved the converted
world without errors.

### Minetest Game worlds

640×192×640 nodes each. Priors were trained on MTG v7 + carpathian and
devtest v7 + carpathian; results are for MTG valleys, v6 and v5 (112,659
blocks the training never saw).

| Codec                           | Size        | vs. default | Compress | Decompress |
|---------------------------------|-------------|-------------|----------|------------|
| zstd (Luanti default, -1)       | 24.63 MB    | 100%        | 11.5 µs  | 8.9 µs     |
| zstd (Luanti level 9)           | 20.69 MB    | 84.0%       | 48.8 µs  | 8.9 µs     |
| **mapblock codec (version 30)** | **6.76 MB** | **27.4%**   | 33.8 µs  | 27.1 µs    |

The codec compresses faster than zstd at Luanti's highest setting and
decompresses about 2–3× slower than zstd. On the simpler devtest game it
reaches 15% of the default size.

### Server performance

Only loading and saving blocks change: the network still uses version 29,
serialized from the in-memory block exactly as before. Measured in the
engine with NodeCore ALPHA and its mods on the NodeCore world, comparing
an unmodified build on the original map ("before") with this branch on the
converted map ("after"). The area loaded is the server's densely built spawn
region, the worst case for the codec.

| 19,200 blocks around spawn                | before  | after    |
|-------------------------------------------|---------|----------|
| loading one block (engine profiler)       | 61 µs   | 121–128 µs |
| of which decoding (`deSer block`)         | 43 µs   | 101–107 µs |
| saving all of them (one periodic save)    | 2.1–2.6 s | 2.4–2.7 s |

Block loading runs on the emerge threads; map saving runs on the server
thread every `server_map_save_interval` seconds. In this area the codec
decodes a block in 70 µs against zstd's 10 µs; over the whole map the
average is 16 µs against 7.6 µs, because most blocks are simple terrain.

### Memory

`mbmemory` on the MTG test worlds. *Retained* is what stays allocated after
the run (per-thread contexts, caches, priors); *peak* is the highest heap use
above the starting point at any moment.

| Operation                  | Retained   | Peak       |
|----------------------------|------------|------------|
| compress zstd (Luanti -1)  | 3581 KiB   | 3587 KiB   |
| compress zstd (Luanti 9)   | 25341 KiB  | 25344 KiB  |
| compress mapblock codec    | 469 KiB    | 667 KiB    |
| decompress zstd            | 2530 KiB   | 2580 KiB   |
| decompress mapblock codec  | 469 KiB    | 582 KiB    |

Of the codec's 469 KiB, 367 KiB are the priors, shared by the whole process.
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

1. **Row prediction.** Nodes are visited z+, y- (top-down), x+. Each row of
   16 nodes is first checked against the row behind and the row above it,
   which makes uniform regions very cheap to code and to decode.
2. **Whole-node prediction.** Otherwise, for each node the distinct values
   among the neighbours left (x-1), behind (z-1) and above (y+1) are
   candidates, and a cascade of binary decisions asks "is this node
   identical (content + param1 + param2) to candidate *k*?". Context: rank,
   which neighbours share the value, whether the diagonal neighbour agrees,
   whether the previous node was a hit, the class of the candidate
   (air / ignore / other). Most nodes cost a small fraction of a bit.
3. **Per-field fallback.** On a miss, content, param1 and param2 are coded
   separately with the same candidate idea. param1/param2 contexts include
   which neighbours have the same content as this node, and param1 gets an
   extra candidate from a **light propagation predictor** (max neighbour light
   − 1, or 15 directly under sunlight). Escapes use binary trees.
4. **Uniform blocks** (about 2/3 of a typical map) cost a few bits plus their
   name.
5. **Header and names** use static trained probabilities: lighting_complete
   0xFFFF as one bit, timestamp as Elias-gamma, palette sorted by name and
   front-coded, built-in tokens for `air`/`ignore`/`unknown`, an order-1
   character model for the rest.
6. **Trained priors.** Adaptive models start every block from probabilities
   measured on training worlds.
7. **Tail** (node metadata, static objects, node timers) is one bit when
   empty, otherwise appended length-prefixed after the arithmetic stream
   (zstd level 7 when smaller; tails are rare but can be large inventories,
   and higher levels are far slower for almost no gain).

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
- Decompression is still 2–3× slower than zstd on average and up to 7× on
  densely built blocks; it is dominated by the arithmetic decoder for blocks
  with mixed content.
- On worlds with many small blocks the database's per-row overhead becomes a
  large part of the file (see the NodeCore numbers).
