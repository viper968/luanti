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

### Per-block savings, without any database overhead

What each block costs inside the database (blob bytes including the version
byte), measured with `mbscan`. The first four worlds were generated with an
unmodified Luanti (version 29), mapgen v7, seed 2026, 640×192×640 nodes
(37,553 blocks each). The priors were trained on the same games but other
seeds and areas (see "Retraining the priors"); none of these worlds were part
of the training set. The NodeCore server row includes the 60,000 blocks of
that world used for training (0.7% of it).

| World                                   | zstd (default) | codec       | saving, all bytes | per-block mean | per-block median |
|-----------------------------------------|----------------|-------------|-------------------|----------------|------------------|
| devtest                                 | 106.4 B/block  | 17.6 B/block | 83.4%            | 86.8%          | 85.2%            |
| Minetest Game                           | 238.4 B/block  | 64.8 B/block | 72.8%            | 81.3%          | 82.5%            |
| Minetest Game + Ethereal NG + Everness  | 291.3 B/block  | 89.9 B/block | 69.1%            | 78.9%          | 78.6%            |
| NodeCore ALPHA                          | 245.8 B/block  | 66.3 B/block | 73.0%            | 81.1%          | 82.5%            |
| VoxeLibre                               | 335.3 B/block  | 114.2 B/block | 65.9%           | 80.2%          | 82.5%            |
| NodeCore server (9.0 M blocks)          | 117.7 B/block  | 34.4 B/block | 70.8%            | 82.7%          | 84.1%            |

The NodeCore server's low per-block cost comes from where its blocks are:
85.6% lie above y = 95 and 11.1% below y = -96, mostly empty air and solid
stone, and only 3.3% in the surface band the other worlds cover. Measured on
the same area as the generated worlds (its built-up spawn region), its
blocks are far larger and the codec saves less:

| NodeCore server slice                     | zstd (default) | codec        | saving, all bytes |
|-------------------------------------------|----------------|--------------|-------------------|
| same area as above (19,200 blocks)        | 561.3 B/block  | 254.2 B/block | 54.7%            |
| whole surface band y -96..95 (258,051)    | 400.9 B/block  | 165.8 B/block | 58.7%            |

The spawn area was kept out of training; the rest of the surface band
overlaps the training sample.

Player builds are less regular than generated terrain, so expect roughly
55–60% in built-up areas, 65–75% on generated terrain and 80–90% for empty
or solid regions; a map's overall figure depends on its mix. About a third of
the bytes in NodeCore's built-up areas are node metadata (numbers stored as
text by the game), which is compressed with zstd.

"All bytes" is the saving on the total size; the per-block figures average
each block's own saving, which weighs the many small uniform blocks (all
air, all stone…) more. Split by kind, uniform blocks shrink by 81–92% and
blocks with mixed content by 60–82%.

### NodeCore server (real, player-built world)

The public world of a long-running NodeCore server (2000³ nodes), 9,003,189
version 29 blocks, measured with `mbscan`. NodeCore was not part of the
training data.

| Codec                           | Block data   | vs. default | Compress | Decompress |
|---------------------------------|--------------|-------------|----------|------------|
| as stored on the server         | 1,043.8 MB   | 99.4%       |          |            |
| zstd (Luanti default, -1)       | 1,050.3 MB   | 100%        | 7.4 µs   | 7.6 µs     |
| zstd (Luanti level 9)           | 925.0 MB     | 88.1%       | 35.6 µs  | 7.6 µs     |
| **mapblock codec (version 30)** | **300.7 MB** | **28.6%**   | 19.5 µs  | 14.8 µs    |

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

640×192×640 nodes each: MTG valleys, v6 and v5 (112,659 blocks the training
never saw).

| Codec                           | Size        | vs. default | Compress | Decompress |
|---------------------------------|-------------|-------------|----------|------------|
| zstd (Luanti default, -1)       | 24.63 MB    | 100%        | 12.6 µs  | 10.5 µs    |
| zstd (Luanti level 9)           | 20.69 MB    | 84.0%       | 52.3 µs  | 10.5 µs    |
| **mapblock codec (version 30)** | **6.69 MB** | **27.2%**   | 33.7 µs  | 32.0 µs    |

The codec compresses faster than zstd at Luanti's highest setting and
decompresses about 2–3× slower than zstd (up to 6× in densely built areas,
see below). On the simpler devtest game it reaches 16% of the default size.

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

These engine measurements predate a later decoder speed-up and retraining:
decoding the spawn area in isolation went from 70 µs to 62 µs per block
since, so the "after" load times are somewhat pessimistic.

Block loading runs on the emerge threads; map saving runs on the server
thread every `server_map_save_interval` seconds. In this area the codec
decodes a block in 70 µs against zstd's 10 µs; over the whole map the
average is 16 µs against 7.6 µs, because most blocks are simple terrain.

### CPU usage

CPU time per block on one core, zstd driven exactly like the engine does.
"Load" is decompression, "save" is compression.

| Map                               | load: zstd | load: codec      | save: zstd default | save: zstd level 9 | save: codec |
|-----------------------------------|------------|------------------|--------------------|--------------------|-------------|
| NodeCore server, whole map (9.0 M)| 7.7 µs     | 14.2 µs (1.8×)   | 6.5 µs             | 31.7 µs            | 20.9 µs     |
| Minetest Game                     | 9.6 µs     | 29.4 µs (3.1×)   | 11.8 µs            | 48.9 µs            | 32.5 µs     |
| VoxeLibre                         | 10.6 µs    | 39.7 µs (3.7×)   | 14.1 µs            | 63.5 µs            | 41.6 µs     |
| NodeCore server spawn (built up)  | 15.9 µs    | 73.1 µs (4.6×)   | 22.2 µs            | 120.6 µs           | 76.4 µs     |

Saving costs about 3× the CPU of zstd's default level and about a third less
than zstd at Luanti's highest level (9).

Whole server process (all threads) with NodeCore ALPHA and its mods, on the
19,200 blocks of the NodeCore spawn area, median of five runs:

| Workload                          | before (v29, zstd) | after (v30, codec) |
|-----------------------------------|--------------------|--------------------|
| start and shut down (baseline)    | 1.09 s             | 1.08 s             |
| load all blocks                   | 2.90 s             | 3.79 s             |
| load, modify and save all blocks  | 4.69 s             | 6.75 s             |

Without the baseline, loading this area takes about 50% more server CPU and
the save about 65% more; this is the worst case, most blocks are cheaper.

Compiling the codec for newer instruction sets (`-march=x86-64-v2`, `-v3`
with AVX2/BMI2, `-v4` with AVX-512, or `native`) made no measurable
difference (all within ±5%, no consistent direction) on an Intel Xeon. The
hot path is a chain of dependent scalar steps (each arithmetic decoding step
needs the result of the previous one) plus hard-to-predict branches, which
wider vector units cannot speed up. The output is identical for all builds.

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

The shipped priors were trained on version 29 worlds generated with
`make_world.sh` (mapgen v7 unless noted): Minetest Game (seeds 12345 and 777,
plus carpathian seed 77), devtest (seed 12345, carpathian seed 77),
VoxeLibre, NodeCore ALPHA and Minetest Game with Ethereal NG and Everness
(seed 777), and 60,000 random blocks of a NodeCore server outside its spawn
area. Node names are a large part of small blocks, so games that are not in
the mix still work but compress somewhat less well.

**The priors are part of the version 30 format.** Retraining them, or changing
anything about the models, makes existing version 30 worlds unreadable unless
it comes with a new serialization version. `TestMapBlock::testLoad30` guards
against accidental changes.

## Finding: transposing blocks for zstd

Independent of the codec, plain zstd (as used for version 29 and for the
network) compresses blocks noticeably better if the node arrays are
reordered first. Measured on the test worlds with zstd at Luanti's default
level and at level 9 (the highest setting):

| World                        | default level | level 9 |
|------------------------------|---------------|---------|
| Minetest Game                | -14.2%        | -8.9%   |
| Minetest Game + biome mods   | -13.1%        | -8.9%   |
| NodeCore ALPHA               | -16.9%        | -11.9%  |
| VoxeLibre                    | -16.7%        | -10.5%  |
| NodeCore server spawn        | -14.9%        | -11.2%  |

The best transform has two parts:

- **Byte split** (most of the gain): store the 16-bit content ids as all high
  bytes, then all low bytes. Most high bytes are zero.
- **Layer order**: store the nodes with y as the slowest axis (y, z, x)
  instead of z, y, x, so each horizontal 16×16 slice is contiguous. Terrain is
  layered, so slices are often uniform.

Other orders (z, x, y; snake/boustrophedon; Morton/Z-order) were worse.
Undoing the transform costs about 4 µs per block with a simple loop. It
would need a new serialization version, and a protocol version for the
network. It does not help the codec, which already predicts from all three
neighbouring directions.

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
