# NodeCore discovery tree

Machine-readable dependency graph of NodeCore's progression, extracted from the
game's own source.

[NodeCore](https://gitlab.com/sztest/nodecore) is a Luanti game with no crafting
grid — everything is built in-world, and progression is tracked by its *hint*
system (`nc_api_hints`). Each hint has:

- a **goal**: the discovery keys that complete it, and
- **reqs**: the discovery keys a player must already hold before the hint is
  even shown.

Chaining one hint's goal to another's reqs gives the discovery tree. That's what
`extract_hints.py` does, writing `nodecore-discovery.json`.

## Regenerating

```sh
pip install lupa
git clone https://gitlab.com/sztest/nodecore.git /path/to/nodecore
python3 extract_hints.py /path/to/nodecore -o nodecore-discovery.json
```

The hints live in `mods/*/hints.lua` as plain `nc.register_hint` calls. Rather
than pattern-matching that Lua with regexes, the script executes those files in
a real Lua interpreter (via `lupa`) against a stub `nc` table, so the extracted
goals and reqs are exactly what the game registers.

The committed JSON was generated from NodeCore at commit `9477a34` (2026-03-19):
**150 hints, 233 edges, 10 roots, 20 tiers.**

## Data model

### Discovery keys

Everything is keyed on *discovery keys*, the strings NodeCore stores per player.
`key_kinds` in the JSON describes each family:

| Kind | Example | Granted by |
| --- | --- | --- |
| `craft` | `assemble staff` | Performing that recipe (stored as `craft:<label>`) |
| `item` | `nc_woodwork:staff` | Holding, seeing, digging or placing that item |
| `event` | `dig:nc_tree:leaves` | A player event: `dig:`, `place:`, `inv:`, `look:`, … |
| `group` | `group:lava` | Discovering any item in that group |
| `toolcap` | `toolcap:cracky:2` | Holding a tool that digs that group at that level |

NodeCore's `expandkey()` (`mods/nc_api_hints/state.lua`) widens each key a
player earns: `dig:nc_tree:leaves` also sets `nc_tree:leaves` and `leaves`, plus
the groups and tool capabilities of any item named in the key.

### JSON layout

| Field | Contents |
| --- | --- |
| `meta` | Source commit/date, counts, per-mod hint totals, recipe-scan stats |
| `key_kinds` | The table above, as data |
| `groups` | Every NodeCore item group and its description, from `nc_api/item_groupdump.lua` |
| `craft_outputs` | Recipe label → items it produces, read from `nc.register_craft` calls |
| `curated_providers` | Hand-resolved keys (see below), each with source citations |
| `nodes` | One entry per hint |
| `edges` | `from` → `to`, with the `via` key that links them |
| `unresolved_req_keys` | Prerequisites no producer could be found for (currently empty) |
| `world_provided_req_keys` | Prerequisites obtained just by playing, with no hint behind them |
| `cycle_edges` | Edges that close a loop, excluded from depth calculation |

Each node carries:

```json
{
 "id": "nc_woodwork/split-a-tree-trunk-into-planks",
 "text": "split a tree trunk into planks",
 "mod": "nc_woodwork",
 "source": "mods/nc_woodwork/hints.lua:36",
 "hidden": false,
 "goal": {"op": "and", "keys": ["split tree to planks"]},
 "reqs": {"op": "or", "keys": ["nc_woodwork:adze", "nc_woodwork:tool_hatchet"]},
 "depends_on": ["nc_woodwork/assemble-an-adze-out-of-sticks", "..."],
 "unresolved_reqs": [],
 "req_key_kinds": ["item"],
 "tier": 3
}
```

`op` is `and` (all keys needed), `or` (any one) or `always` (no prerequisite —
a root). `tier` is depth from the roots: an `and` hint waits for its slowest
prerequisite, an `or` hint for its fastest. `hidden` marks hints NodeCore never
displays as an upcoming goal.

Every edge records how it was resolved:

- `direct` — the parent's goal key is literally the child's req key (125 edges).
- `curated` — resolved through `curated_providers` (99 edges).
- `implied` — an `inv:X`/`dig:X` req matched a parent that merely yields `X`.
  The req is stricter than the goal, but the hint that first gets you the item
  is still its real prerequisite (9 edges).

## Accuracy and its limits

Hints, goals and reqs are exact: they come from executing the game's own Lua.
Two things are reconstructed rather than read, because they only exist once the
game is running:

1. **Group and tool-capability keys.** `expandkey()` resolves these against the
   live item registry. The keys no hint goal produces are mapped by hand in
   `CURATED_PROVIDERS`, each entry citing the source lines that assign the group
   or tool capability.
2. **Recipe outputs.** Hint goals name a recipe by its *label* while other hints'
   reqs name the *item* it yields, so `scan_crafts()` reads each
   `nc.register_craft` call's `replace` and `items` fields. 20 of 92 recipes
   build their label or outputs at load time and can't be read statically; those
   are covered by `CURATED_PROVIDERS` too.

For ground truth on both, run NodeCore headless and dump `core.registered_items`
alongside `nc.hints`. That was not done here — a curated map traceable to source
lines was preferred over a full engine build.

Three `cycle_edges` are genuine loops in NodeCore's own progression (chips
pack back into cobble, and stone tools need cobble that stone tools help dig —
the wooden tool path is what actually breaks that one). They're excluded from
depth calculation and flagged so a viewer can draw them differently.
