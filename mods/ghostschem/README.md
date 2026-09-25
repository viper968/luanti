# Ghost Schematics

Schematic copy/paste for Luanti where you **see what you are about to place
before you place it**, including what it would overwrite — instead of pasting
first and reaching for undo afterwards.

The preview is drawn as translucent "ghost blocks" floating in the world. They
are entities, not nodes: nothing is written to the map until you commit.

```
/gswand                 get the wand and the placer
                        wand: punch = corner 1, place = corner 2
/gs copy                copy the selection to the clipboard
                        placer: place = move the ghost here
                                sneak+place = rotate 90 degrees
                                punch = commit
/gs undo                revert the last commit

/gs load <name>         load worldpath/schems/<name>.mts or .json
/gs info                describe the clipboard and anything the import dropped
```

Ghost colouring tells you what will actually happen:

| colour | meaning |
|---|---|
| plain node texture | lands in empty space |
| red | something is there and **will be overwritten** |
| amber | something is there and **will be skipped** (`force_placement` off) |
| violet | target area not loaded, so it cannot be classified yet |

## Why it is built this way

Three engine constraints shape the whole design. They are worth writing down,
because none of them are obvious and all of them are load-bearing.

### 1. The client map is server-authoritative

`ClientMap` renders `MapBlockMesh`es built from server-sent `MapBlock`s. There
is no client-side overlay layer, and neither CSM (`doc/client_lua_api.md`) nor
SSCSM can create geometry — SSCSM cannot even load mods yet
(`doc/sscsm_api.md`: *"Currently, you can not add any mods"*). So real ghost
*nodes* would require an engine change. Entities are the only geometry a mod
can add.

### 2. Entity opacity can only come from the texture

The object fragment shader ends with:

```glsl
col = vec4(col.rgb, base.a);   // object_shader/opengl_fragment.glsl:465
```

Output alpha comes **only** from the texture. Vertex-colour alpha is already
spent carrying the day/night sunlight ratio:

```glsl
nightRatio = 1.0 - color.a;    // object_shader/opengl_vertex.glsl:146
color.a = 1.0;
```

So translucency has to be baked into the texture string with `^[opacity:N`,
and that only works for visuals whose textures *are* strings.

### 3. Which forces `visual = "cube"`

| | shape fidelity | translucent? |
|---|---|---|
| `visual = "cube"` | full cubes only | **yes** — textures are plain strings and `GenericCAO::updateTextures` applies texture modifiers to them (`content_cao.cpp:1351`) |
| `visual = "node"` | correct nodeboxes, stairs, slabs, param2 | **no** — `updateTextures` has no `OBJECTVISUAL_NODE` branch (`content_cao.cpp:1310`), so `set_texture_mod` is silently ignored, and textures come from the nodedef |

Translucency matters more than stair shapes for a preview, so: cube. See
*Upgrading to `visual = "node"`* below for the small engine patch that removes
this trade-off.

## Importing from external builders

`/gs load <name>` reads `worldpath/schems/<name>.mts` or `<name>.json`. JSON is
dispatched on its `format` field through `ghostschem.json_importers`, so
another builder's format can be added without touching the loading code.

### `nodecore-optics-schematic` v1

```json
{ "format": "nodecore-optics-schematic", "version": 1,
  "name": "Staff builder", "description": "...",
  "terrain": {"mode": "off", "top": -1, "depth": 3, "node": "nc_terrain:stone"},
  "nodes": [ {"pos": [5, 1, 0], "name": "nc_woodwork:form",
              "param2": 3, "stack": {"name": "nc_tree:stick", "count": 100}} ] }
```

Three things differ from a Luanti schematic and have to be reconciled:

**It is sparse.** Only listed positions hold nodes, so every unlisted cell in
the bounding box becomes air. The reference fixture is 74% air (235 nodes in a
14×5×13 box).

**Coordinates are signed** and centred on the builder's own origin — the
reference fixture spans x −3..10, y −1..3, z −6..6 — so they are translated to
a 0-based dense array. A paste therefore anchors its *minimum corner* at your
cursor, like WorldEdit. The local coordinate the builder called `(0,0,0)` is
reported by `/gs info` as `origin_offset`, so anchoring on the builder's origin
instead is a small addition on top.

**Nodes may carry an item `stack`,** and Luanti schematics cannot store node
inventories at all — neither `.mts` nor `place_schematic` has anywhere to put
them. Stacks therefore ride along on the node entries (`place_schematic`
ignores the extra field: `read_schematic_def` reads only `name`, `param1`/
`prob`, `param2` and `force_place`) and are written after placement.

That last part has a catch worth knowing about:

> `Schematic::placeOnMap()` blits through a `VoxelManip` and dispatches a map
> edit event, but it **never runs `on_construct`**. A container placed by a
> schematic therefore has no inventory lists at all, because those are created
> by the node's own `on_construct`. `gs.apply_stacks` re-sets each
> stack-carrying node with `core.set_node` — which does run the callbacks —
> before writing the stack. It first checks the node actually landed, so a
> `force_placement = off` paste is never quietly forced.

The target inventory list is discovered at runtime from the placed node rather
than guessed per game: one list is used directly, `main` wins when there are
several, and genuinely ambiguous cases are reported rather than picked at
random. Anything that could not be written is named in the placement message.

**Terrain modes are not guessed.** `"off"` is honoured exactly; any other mode
is skipped and reported, because the builder's terrain semantics are not
documented anywhere checkable and a preview that guesses is worse than one that
says it does not know. Tell the importer what the other modes mean and it is a
few lines in `formats.lua`.

`/gs info` also names any node types the running game does not have, since
those cannot be placed — useful when a NodeCore schematic meets a world without
NodeCore.

### Converting to `.mts`

`tools/json2mts.js` turns a builder's JSON into a standard `.mts`, usable by
WorldEdit, mapgen decorations and `core.place_schematic` on servers that do not
run this mod. It is one dependency-free file that works two ways.

From the command line (Node 18+):

```sh
node tools/json2mts.js staff-builder.schematic.json            # -> staff-builder.mts
node tools/json2mts.js staff-builder.schematic.json out.mts --keep-existing
```

Or as an export button in the builder itself. Add the script and call
`download` with the builder's current schematic object:

```html
<script src="json2mts.js"></script>
<button onclick="GhostschemMts.download(schematicJson, 'staff-builder.mts')">
  Export .mts</button>
```

`GhostschemMts.convert(doc)` returns `{bytes, report}` instead, if the page
wants to handle the file itself. The file is safe to paste inline into a page
too; a spec checks it never contains a literal `</script`, because the HTML
parser ends a script block at the first one it meets, even inside a comment.

Both uses go through `CompressionStream`, so the command line runs exactly the
code a browser does. The browser path was verified in Chromium: the downloaded
file decodes identically to the command-line output.

The byte layout comes from the engine's own writer, `serializeToMts()`, not
from third-party docs, and `/gs selftest` checks the output with the engine's
own reader: `core.read_schematic` must see every cell of the converted file
exactly as the in-game JSON importer does.

**Item stacks cannot be converted.** `.mts` has no field for node inventories,
so the converter lists every stack it had to leave out:

```
WARNING: .mts cannot store node inventories, so 3 item stack(s) are NOT in this file:
  1 x nc_woodwork:staff in nc_woodwork:form at (5,0,1)
  1 x nc_woodwork:tool_hatchet in nc_woodwork:form at (5,1,0)
  100 x nc_tree:stick in nc_woodwork:form at (1,2,0)
```

Keep the `.json` and use `/gs load` when the stacks matter.

**Gaps.** By default, cells the JSON does not list become air and clear
whatever is in the world, which matches `/gs load` and keeps a machine's
internal gaps open (sticks falling through a tube, beams crossing a cell).
`--keep-existing` writes them with probability 0 instead, which the engine
skips (`MTSCHEM_PROB_NEVER` in `blitToVManip`), so existing terrain shows
through.

## Correctness notes

These are the places where guessing would produce a preview that *lies*, which
is worse than no preview at all. Each is derived from engine source and
covered by a test in `tests/`.

**Schematic array layout is X-fastest.** From `blitToVManip()`:
`xstride = 1`, `ystride = size.X`, `zstride = size.X * size.Y`, and
`l_read_schematic()` pushes `data[i+1] = schemdata[i]`. So the index is
`1 + x + y*sx + z*sx*sy` — *not* the z-major order most voxel formats use.

**Rotation matches the engine exactly.** The mappings in `api.lua` are derived
from `blitToVManip`'s `i_start` / `i_step_x` / `i_step_z`:

```
"90"  (x, z) -> (z,          sx - 1 - x)   size -> (sz, sy, sx)
"180" (x, z) -> (sx - 1 - x, sz - 1 - z)   size unchanged
"270" (x, z) -> (sz - 1 - z, x)            size -> (sz, sy, sx)
```

`tests/rotation_spec.lua` checks these against a direct transcription of the
C++ index arithmetic, on deliberately asymmetric sizes so a transposed axis
cannot pass by accident. It also asserts the rotation is a bijection: a hole in
`data` would make `place_schematic`'s `for_ipairs` silently truncate.

**`force_placement` defaults to `true`.** `l_mapgen.cpp:1748` sets
`bool force_placement = true` before reading the optional argument. Pasting
overwrites by default.

**With `force_placement` off, only air and ignore are replaceable.** The engine
test is literally `if (c != CONTENT_AIR && c != CONTENT_IGNORE) continue;`. It
does **not** honour `buildable_to`, so grass and flowers block placement even
though normal node placement would replace them. `preview.lua` classifies
conflicts the same way.

**Schematics are kept as tables, never filenames.** `core.place_schematic`
permanently caches anything loaded from a file — *"The only way to load the
file anew is to restart the server"*. Reading into a table with
`core.read_schematic` and placing from that table sidesteps the cache, which
matters in an edit-preview-place loop. Round-tripping is lossless:
`read_schematic` writes `prob = probability * 2` and `read_schematic_def` does
`param1 >>= 1`.

**`core.load_area` does not generate map.** It says so explicitly: *"This
function does not trigger map generation."* Previewing into terrain nobody has
visited therefore classifies every node as unknown, which is honest but
useless. `preview.lua` notices this and fires `core.emerge_area`, rebuilding
the preview from its callback. The emerge is keyed on the preview's bounds, so
moving or rotating re-arms it while the rebuild it triggers does not loop.

**Ghosts can never be left behind.** `static_save = false` means they are never
written into a mapblock. That also means `max_objects_per_block` never applies,
since it only gates *stored* objects (`MapBlock::saveStaticObject`).

## Performance

One entity per node gets expensive fast, so interior nodes are culled: a ghost
is only spawned if the node is not fully enclosed by opaque nodes *within the
schematic*. A solid 16×16×16 schematic is 4096 nodes but only **1352** ghosts —
about a 3× reduction, and it grows with volume.

Above `ghostschem_max_entities` (default 3000) the preview falls back to a
bounding box, still coloured by whether the paste would overwrite anything.

| setting | default | meaning |
|---|---|---|
| `ghostschem_opacity` | `110` | ghost alpha, 0–255 |
| `ghostschem_max_entities` | `3000` | above this, draw an outline instead |
| `ghostschem_undo_depth` | `10` | undo steps kept per player |
| `ghostschem_max_import_volume` | `524288` | largest bounding box an import may expand to |
| `ghostschem_selftest` | `false` | enable `/gs selftest` (registers no nodes in this build, but keeps the test code out of production servers) |
| `ghostschem_selftest_on_start` | `false` | run the suite on server start, then shut down (for CI) |

## Undo

Undo snapshots the target region with a `VoxelManip` rather than
`core.create_schematic()`, which requires a filename (`luaL_checkstring` on
argument 4) and would mean a disk write per paste.

Two caveats, both acceptable for what is being undone: `get_data()` returns
content IDs which are only stable within one server run, and `VoxelManip` does
not carry node metadata or inventories — but `place_schematic` does not write
metadata either.

## Upgrading to `visual = "node"`

The cube restriction comes from one line of shader code. To lift it:

1. Add `opacity` to `ObjectProperties` (`src/object_properties.h`), plus
   serialize/deserialize and the Lua read in `c_content.cpp`.
2. Force `ALPHAMODE_BLEND` for the object's tiles when `opacity < 1`.
3. Change the shader's final line to
   `col = vec4(col.rgb, base.a * objectOpacity);`

Both `cube` and `node` visuals go through `object_shader` — `visual = "node"`
reaches it via `getAdHocNodeShader(mat, shdsrc, "object_shader", alpha_mode, ...)`
in `content_cao.cpp:245` — so **one shader line covers both**.

With that patch, switch `ghost.lua` to `visual = "node"` with
`node = {name = ..., param2 = ...}` and ghosts gain correct nodebox shapes.
`api.lua`'s `gs.rotate` would then also need to rotate `param2`, which it
currently leaves to the engine (see the comment there).

The larger version — one mesh for an entire schematic rather than one entity
per node — is also within reach: `MeshMakeData` already owns its own
`VoxelManipulator` (`src/client/mapblock_mesh.h:34`) and already has
`fillSingleNode()` for exactly the "voxel data that is not from the map" case,
used by `wieldmesh.cpp:362` and `content_cao.cpp:216`. A `fillFromSchematic()`
beside it, run through `MapblockMeshGenerator`, would produce a single mesh per
schematic the same way `generateNodeMesh()` does for one node.

## Tests

```sh
tests/check.sh
```

Syntax-checks every file and runs two standalone specs that stub `core` and
exercise `api.lua` directly, so they need no server and no build:

- `tests/rotation_spec.lua` — rotation against a transcription of `blitToVManip`
- `tests/culling_spec.lua` — occlusion culling, air, glass, `prob = 0`
- `tests/import_spec.lua` — JSON import: bounding boxes, sparseness, signed
  coordinates, `param2`, stacks surviving rotation, and every rejection path
- `tests/json2mts_spec.js` — the converter, decoded independently with
  `node:zlib`: header, layout, `--keep-existing`, committed fixtures still
  current, inline safety, the CLI (skipped if `node` is not installed)

**Check syntax with LuaJIT, not `luac5.1`.** Lua 5.1's reference lexer silently
accepts unknown escape sequences such as `"\."` (it drops the backslash and
keeps the character), while LuaJIT rejects them with *"invalid escape
sequence"*. Most Luanti builds ship LuaJIT, so a file can pass `luac5.1 -p`
and still fail to load in-game. `check.sh` prefers `luajit` for exactly this
reason, and warns if it has to fall back.

The in-engine suite checks the same claims against the *live* engine, which is
the part that actually matters — it proves the preview is not lying:

```
ghostschem_selftest = true      # in minetest.conf, then in-game:
/gs selftest
```

or headlessly, for CI (runs on server start, then shuts down):

```
ghostschem_selftest = true
ghostschem_selftest_on_start = true
```

```
Ghost schematic self test: all 40 checks passed
  ok   rot 0/90/180/270  preview matches place_schematic
  ok   force=on / force=off conflict classification
  ok   commit wrote the schematic; undo restored the region exactly
  ok   oversized schematic falls back to an outline on the 12 box edges
  ok   .mts export / listing / read-back round trip
  ok   import the reference JSON (14x5x13, 235 nodes, 3 item stacks)
  ok   /gs load finds a .json by name, and it appears in the listing
  ok   apply_stacks constructs the node and writes the stack into its inventory
  ok   json2mts staff-builder.mts: engine reads all 910 cells as the JSON import does
  ok   json2mts default mode clears gaps; --keep-existing leaves them alone
  ok   test volume restored exactly (0 of 245760 nodes differ)
  ok   preview emerged ungenerated map and reclassified (1 -> 0)
```

The suite snapshots its whole test volume with a `VoxelManip`, restores it
afterwards, and then checks that the restore was exact. Every section also
declares the region it writes, and the run fails if any of them falls outside
the snapshot, so a new section cannot silently leave a test build in the
world.
