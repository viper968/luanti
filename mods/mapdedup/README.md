# mapdedup

Shrinks a world by deleting mapchunks that nothing but the map generator has
ever changed. Deleted chunks regenerate the next time someone goes there.

## How it decides

When a mapchunk is generated, the mod stores a fingerprint of every mapblock
the generator wrote to. That is the chunk itself plus the one-block border
shell around it, because decorations, caves and dungeons spill into
neighbours. Each fingerprint covers node names and param2, and node metadata
when it is available. Light (param1) is ignored.

A check compares a chunk and its border shell against those fingerprints. The
chunk is deleted only if every block still matches. That means no player,
mod, liquid, falling node or ABM has changed anything there since the
generator last wrote it.

The mod never compares against a fresh regeneration of the chunk, so mapgen
non-determinism can't make a modified chunk look untouched. If anything
differs, the chunk is kept.

It always works on whole chunks, because the engine regenerates a deleted
block by regenerating its entire chunk and writing into the border shell
around it. `core.delete_area` on a single block can therefore change the
blocks next to it.

## Usage

Needs the `server` privilege.

* `/mapdedup check [<radius>|all]`: dry run. Reports how many chunks could be
  deleted and why the others are kept. `<radius>` is in mapchunks around you
  (default 2); `all` covers every chunk the mod has recorded.
* `/mapdedup clean [<radius>|all]`: the same, but deletes the deletable chunks.
* `/mapdedup status`, `/mapdedup stop`

Jobs run in the background with a per-step time budget. The reasons a chunk
is kept are:

* `modified`: something other than the generator changed it or its border.
* `player_near`: a player is within `mapdedup.player_margin` nodes.
* `has_objects`: active entities or dropped items are inside.
* `mapgen_changed`: it was generated under different mapgen settings or
  biome, ore or decoration registrations than the current ones. Regenerating
  it would not match its neighbours.
* `untracked`: it was generated before the mod was installed.

**The map file does not shrink by itself.** SQLite keeps freed pages and reuses
them for new blocks. To shrink the file, stop the server and run
`sqlite3 map.sqlite "VACUUM;"`. Back up the world first.

## Settings

* `mapdedup.record_in`: where new chunks are fingerprinted.
  * `mapgen` (default) fingerprints in the mapgen threads, at about 1 ms of
    main-thread time per chunk. Anything that changes right after generation
    (water settling, other mods' `on_generated` callbacks such as dungeon loot
    chests) counts as a modification, so fewer chunks qualify. In testing,
    about half as many chunks qualified as with `server`.
  * `server` fingerprints on the main thread after every `on_generated`
    callback. It finds more deletable chunks but costs about 50 ms per
    generated chunk, which can cause lag while players explore.
* `mapdedup.player_margin` (default 64), `mapdedup.step_budget` (ms per server
  step, default 30), `mapdedup.debug`.

## Known limitations

* **Chunks generated before the mod was installed are never deleted.** No
  fingerprint exists for them.
* **Seams.** A regenerated chunk is generated after its neighbours instead of
  before them, so features that crossed the border can come out differently.
  Typical cases are a cave tunnel ending at the chunk edge, a dungeon or tree
  cut off or added. In testing, about 0.03% of nodes changed, all within 16
  nodes of a chunk boundary. This is generation-order dependence rather than
  randomness.
* **Building next to a deleted chunk.** When a deleted chunk regenerates, it can
  carve caves or dungeons into the one-block border of the chunks next to it.
  Normal exploration does the same thing at the edge of unexplored land.
* **Inactive objects aren't seen.** Entities stored in an unloaded block, such
  as a mob, are invisible to Lua. If the chunk is otherwise untouched, they are
  deleted with it. Anything a player built or dug is a node change, so those
  chunks are kept.
* In `mapgen` mode, a mod that calls `core.set_gen_notify({custom = false})`
  stops fingerprints from arriving. The affected chunks are then never deleted
  and a warning is logged.
* Changing `chunksize` discards all records.
