# Experimental mapblock codec

A research prototype of a custom compression algorithm for Luanti mapblocks,
benchmarked against the zstd compression Luanti uses today. This is **not**
wired into the engine; it is a standalone benchmark that reads real
`map.sqlite` files.

## Results

Train worlds: devtest with mapgen v7, carpathian, v5 (112,659 blocks).
Test worlds (never seen during training): devtest with mapgen v6, valleys, fractal (112,659 blocks).
Single thread, `g++ -O2`, times are per block.

| Codec                         | Size      | vs. default | Compress | Decompress |
|-------------------------------|-----------|-------------|----------|------------|
| zstd -3 (Luanti default `-1`) | 13.45 MB  | 100%        | 143 µs   | 32 µs      |
| zstd -9 (Luanti max setting)  | 11.31 MB  | 84.1%       | 105 µs   | 32 µs      |
| zstd -19                      | 10.31 MB  | 76.7%       | 717 µs   | 31 µs      |
| **this codec**                | **2.05 MB** | **15.2%** | **37 µs** | **27 µs** |

Every test block round-trips losslessly (palette order is canonicalised, which
is not observable by the engine since local ids are remapped by name on load).

Caveat: devtest terrain is simple (few node types, no ores/plants/structures),
so real game worlds will compress less dramatically. Run it on your own
worlds before drawing conclusions.

## How it works

zstd sees a mapblock as a flat byte string. This codec instead models the 3D
structure directly and codes every decision with a binary arithmetic coder
(lpaq-style, 16-bit probabilities, count-based adaptive learning rate).

1. **Whole-node prediction.** Nodes are scanned z→, y↓ (top-down), x→. For
   every node, the distinct values among the causal neighbours left (x-1),
   behind (z-1) and above (y+1) are candidates. A cascade of binary decisions
   asks "is this node identical (content+param1+param2) to candidate *k*?",
   with context: rank, which neighbours share the value, whether the diagonal
   neighbour agrees, whether the previous node was a hit, and the class of the
   candidate (air / ignore / other). Most nodes cost a small fraction of a bit.
2. **Per-field fallback.** On a miss, content, param1 and param2 are coded
   separately with the same candidate idea. param1/param2 contexts include
   *which neighbours have the same content* as this node, and param1 gets an
   extra candidate from a **light propagation predictor** (max neighbour light
   − 1, or 15 directly under sunlight; top-down scan makes "above" causal).
   Escapes are coded with binary trees conditioned on node class.
3. **Mono blocks** (all nodes identical, ~2/3 of a typical map) cost a few bits
   plus their name.
4. **Header and names** go through the same coder with static, trained
   probabilities: the 0xFFFF `lighting_complete`, timestamp as Elias-gamma,
   palette sorted by name and front-coded (shared prefix with previous name),
   builtin tokens for `air`/`ignore`/`unknown`, order-1 character model.
5. **Trained priors.** All models start each block from probabilities
   measured on training worlds (adaptive models with zero confidence, which
   tuned best), so blocks don't pay to re-learn obvious facts.
6. **Tail** (node metadata, static objects, node timers) is a single flag when
   empty, otherwise appended after the arithmetic stream (zstd-compressed if
   smaller). The decoder knows exactly where the arithmetic stream ends.

Remaining cost on the test set: whole-node structure 1.15 MB, names 0.33 MB,
header (mostly timestamps) 0.16 MB, per-field fallback 0.32 MB.

## Running it

Dependencies: zstd and sqlite3 dev headers.

```sh
g++ -O2 -std=c++17 util/mapblock_codec/bench.cpp -o mbbench -lzstd -lsqlite3

# generate worlds (needs a built luantiserver; run from the repo root)
util/mapblock_codec/make_world.sh bin/luantiserver /tmp/w_v7 devtest v7 12345
util/mapblock_codec/make_world.sh bin/luantiserver /tmp/w_v6 devtest v6 999

# train on the first list, measure on the second (comma separated)
./mbbench /tmp/w_v7/map.sqlite /tmp/w_v6/map.sqlite
# optional: ./mbbench TRAIN TEST <prior confidence> <adaptation limit>
# NOZSTD=1 skips the (slow) zstd baselines
```

Only version-29 blocks (current format) are read.

## Open questions before engine integration

- New `SER_FMT_VER` (30) for disk only, or also the network protocol?
- The priors table becomes part of the format; it must be versioned and should
  be trained on a broad mix of games, not just devtest.
- Node names are now the second largest cost. A world-wide name table would
  remove most of it but breaks the "every block is self-contained" property.
- Hardening the decoder against malicious/corrupt input (bounds on palette
  size, name lengths, tail size) before it touches network data.
