#!/usr/bin/env python3
"""Extract NodeCore's discovery tree from NodeCore game source.

NodeCore's progression system is its "hints": each hint has a *goal* (the
discovery keys that complete it) and *reqs* (the discovery keys a player must
already hold before the hint is even shown).  Chaining goals to reqs yields the
discovery tree.

The hints themselves live in `mods/*/hints.lua` as plain `nc.register_hint`
calls.  Rather than pattern-matching that Lua with regexes, this script runs it
in a real Lua interpreter (via lupa) with a stub `nc` table, so the extracted
goals/reqs are exactly what the game registers.

Usage:
    pip install lupa
    python3 extract_hints.py /path/to/nodecore -o nodecore-discovery.json

NodeCore lives at https://gitlab.com/sztest/nodecore.
"""

import argparse
import collections
import datetime
import json
import os
import re
import subprocess
import sys

try:
    from lupa import LuaRuntime
except ImportError:
    sys.exit("lupa is required: pip install lupa")


# ---------------------------------------------------------------------------
# Curated providers.
#
# A hint's reqs are discovery keys.  Most are produced directly by some other
# hint's goal, which gives us an edge for free.  The rest are `group:` and
# `toolcap:` keys that NodeCore resolves at runtime through nc_api_hints'
# expandkey(): discovering any item grants that item's groups and tool
# capabilities as keys too.  Reproducing that needs the live item registry, so
# the handful of keys no hint goal produces are mapped here by hand, each traced
# to the source lines that assign the group / tool capability.
#
# Each entry maps the unresolved key to the goal keys of the hints that get you
# an item carrying it.  "world" entries are keys a player picks up just by
# looking at the world, with no hint prerequisite at all.
# ---------------------------------------------------------------------------
CURATED_PROVIDERS = {
    "group:chisel": {
        "provided_by": [
            "anvil making lode bar",
            "anvil making lode rod",
            "anvil making lode ladder",
        ],
        "note": "Lode bars/rods/ladders are the metal shafts usable as chisels.",
        "source": [
            "mods/nc_lode/shafts.lua:21 (bar, chisel=1)",
            "mods/nc_lode/shafts.lua:79 (rod, chisel=2)",
            "mods/nc_lode/ladder.lua:31 (ladder, chisel=2)",
        ],
    },
    "group:concrete_powder": {
        "provided_by": ["mix aggregate", "mix render", "mix mud", "mix cloudmix"],
        "note": "Applied by the concrete API to every dry mix powder.",
        "source": ["mods/nc_concrete/api.lua:14"],
    },
    "group:dirt_raked": {
        "provided_by": ["rake dirt"],
        "source": ["mods/nc_writing/leaching.lua:69-70"],
    },
    "group:humus_raked": {
        "provided_by": ["rake humus"],
        "source": ["mods/nc_writing/leaching.lua:74-75"],
    },
    "group:door": {
        "provided_by": ["door pin plank", "door pin cobble"],
        "note": "A panel only becomes a hinged door once pinned.",
        "source": ["mods/nc_doors/register.lua"],
    },
    "group:optic_gluable": {
        "provided_by": ["cleave lenses from glass", "hammer prism from glass"],
        "note": "Lenses and prisms are the gluable optics.",
        "source": ["mods/nc_optics/lens.lua:57", "mods/nc_optics/glued.lua:12-13"],
    },
    "group:peat_grindable_item": {
        "provided_by": [
            "nc_tree:leaves_loose",
            "nc_tree:eggcorn",
            "nc_flora:rush",
            "group:flora_sedges",
        ],
        "note": "Loose dead plant matter: dry leaves, eggcorns, rushes, sedges.",
        "source": [
            "mods/nc_tree/node.lua:117",
            "mods/nc_tree/grow_node.lua:25",
            "mods/nc_flora/rushes.lua:53",
            "mods/nc_flora/sedges.lua:56",
        ],
    },
    "group:peat_grindable_node": {
        "provided_by": ["pack thatch", "pack wicker"],
        "source": ["mods/nc_flora/thatch.lua:15", "mods/nc_flora/wicker.lua:16"],
    },
    "group:rakey": {
        "provided_by": ["assemble rake", "assemble lode rake", "pack thatch", "pack wicker"],
        "note": "Rakes proper, plus thatch/wicker which rake while wielded.",
        "source": [
            "mods/nc_woodwork/rake.lua:23",
            "mods/nc_lode/rake.lua",
            "mods/nc_flora/thatch.lua:36",
            "mods/nc_flora/wicker.lua:38",
        ],
    },
    "group:silica_lens": {
        "provided_by": ["cleave lenses from glass"],
        "source": ["mods/nc_optics/lens.lua:52"],
    },
    "group:smoothstone": {
        "world": True,
        "note": "Plain stone carries this group and is visible everywhere "
                "underground, so looking at it grants the key with no hint behind it.",
        "source": ["mods/nc_terrain/node.lua:68", "mods/nc_lode/ore.lua:38"],
    },
    "group:totable": {
        "provided_by": ["assemble wood shelf", "assemble lode shelf"],
        "note": "Shelves/crates are what a tote can pick up.",
        "source": ["mods/nc_woodwork/shelf.lua:19", "mods/nc_lode/shelf.lua:15"],
    },
    # Tool capability tiers.  nc.toolcaps({group = N}) fills times[1..N], so a
    # `toolcap:g:N` key needs a tool of level >= N in that dig group.
    # Wooden tools are level 2, stone-tipped 3, lode 4 (annealed) / 5 (tempered).
    # Only assembled tools count: expandkey reads tool_capabilities, not
    # tool_head_capabilities, so tool heads alone do not grant these.
    "toolcap:choppy:2": {
        "provided_by": ["assemble wood hatchet", "assemble nc_stonework:tool_hatchet"],
        "note": "Wooden hatchet (level 2) or better.",
        "source": ["mods/nc_woodwork/tools.lua:76", "mods/nc_stonework/tools.lua:40"],
    },
    "toolcap:choppy:4": {
        "provided_by": ["forge lode toolhead_mallet", "metallurgize nc_lode:block_annealed"],
        "note": "Needs an assembled lode hatchet (annealed level 4). No hint "
                "covers the 'assemble lode hatchet' craft itself, so this points "
                "at the forging/annealing steps that lead to it.",
        "source": ["mods/nc_lode/tools.lua:40", "mods/nc_lode/tools.lua:71"],
    },
    "toolcap:cracky:2": {
        "provided_by": ["assemble wood pick", "assemble nc_stonework:tool_pick"],
        "note": "Wooden pick (level 2) or better.",
        "source": ["mods/nc_woodwork/tools.lua:79", "mods/nc_stonework/tools.lua:41"],
    },
    "toolcap:crumbly:2": {
        "provided_by": ["assemble wood spade", "assemble nc_stonework:tool_spade"],
        "note": "Wooden spade (level 2) or better.",
        "source": ["mods/nc_woodwork/tools.lua:72", "mods/nc_stonework/tools.lua:39"],
    },
    "toolcap:thumpy:3": {
        "provided_by": ["assemble nc_stonework:tool_mallet"],
        "note": "Wooden mallets are only level 2; a stone-tipped mallet "
                "(level 3) is the first tool that satisfies this.",
        "source": ["mods/nc_stonework/tools.lua:38", "mods/nc_api/util_toolcaps.lua:19"],
    },

    # Item keys whose producing recipe scan_crafts() cannot read statically,
    # because the recipe is generated at load time (concrete mixes, lode
    # metallurgy tempers) or the item is produced by an ABM rather than a craft.
    "nc_woodwork:form": {
        "provided_by": ["wooden frame to form"],
        "source": ["mods/nc_woodwork/frame.lua"],
    },
    "nc_tree:humus": {
        "provided_by": ["peat compost"],
        "note": "Humus is what fermenting a peat block yields.",
        "source": ["mods/nc_tree/compost.lua"],
    },
    "nc_fire:lump_coal": {
        "provided_by": ["chop nc_fire:coal1", "chop nc_fire:coal2",
                        "chop nc_fire:coal3", "chop nc_fire:coal4"],
        "note": "Coal lumps come from chopping a charcoal node down.",
        "source": ["mods/nc_fire/charcoal.lua"],
    },
    "nc_concrete:aggregate": {
        "provided_by": ["mix aggregate"],
        "note": "Concrete mix recipes are generated by the concrete API, so the "
                "recipe scan cannot read their outputs statically.",
        "source": ["mods/nc_concrete/api.lua"],
    },
    "nc_sponge:sponge_wet": {
        "provided_by": ["nc_sponge:sponge"],
        "note": "A dry sponge becomes wet by absorbing water via ABM, not by a "
                "recipe, so the dry sponge is the real prerequisite.",
        "source": ["mods/nc_sponge/abm.lua:30"],
    },
    "lode anvil": {
        "provided_by": ["anvil:hot/annealed", "anvil:hot/tempered"],
        "note": "Discovered on completing any craft atop a lode anvil.",
        "source": ["mods/nc_lode/anvils.lua:120"],
    },
    "nc_lode:prill_hot": {
        "provided_by": ["lode cobble drain"],
        "note": "Draining lode cobble is what yields glowing prills.",
        "source": ["mods/nc_lode/oresmelt.lua"],
    },
    "nc_lode:prill_annealed": {
        "provided_by": ["metallurgize nc_lode:block_annealed", "lode cobble drain"],
        "note": "Annealing applies to any lode item; the hints only name the "
                "cube, so this points at the same annealing step.",
        "source": ["mods/nc_lode/metallurgy.lua"],
    },
    "nc_lode:prill_tempered": {
        "provided_by": ["metallurgize nc_lode:block_tempered", "lode cobble drain"],
        "note": "As above, for tempering.",
        "source": ["mods/nc_lode/metallurgy.lua"],
    },
    "nc_lode:frame_annealed": {
        "provided_by": ["anvil making lode frame", "metallurgize nc_lode:block_annealed"],
        "source": ["mods/nc_lode/shafts.lua", "mods/nc_lode/metallurgy.lua"],
    },
    "nc_lode:form": {
        "provided_by": ["lode frame_annealed to form"],
        "source": ["mods/nc_lode/shelf.lua"],
    },
    "forge lode toolhead_pick": {
        "provided_by": ["forge lode toolhead_mallet"],
        "note": "One 'forge lode <head>' recipe exists per tool head; the hints "
                "only name the mallet.",
        "source": ["mods/nc_lode/tools.lua:75-78"],
    },
    "nc_lode:tool_mallet_tempered": {
        "provided_by": ["metallurgize nc_lode:toolhead_mallet_tempered"],
        "note": "Temper the head, then assemble it onto a staff.",
        "source": ["mods/nc_lode/tools.lua:34-52"],
    },
    "nc_lode:tool_hatchet_tempered": {
        "provided_by": ["metallurgize nc_lode:toolhead_hatchet_tempered",
                        "metallurgize nc_lode:toolhead_mallet_tempered"],
        "note": "As above; the tempering hint only names the mallet head.",
        "source": ["mods/nc_lode/tools.lua:34-52"],
    },
}

# Keys granted by ordinary play/observation rather than by completing a hint.
WORLD_KEYS = {k for k, v in CURATED_PROVIDERS.items() if v.get("world")}

# nc_player_gui/hints.lua renders the hint list; it registers no hints itself.
SKIP_MODS = {"nc_player_gui"}

EVENT_PREFIXES = ("dig", "inv", "place", "look", "punch", "die", "spawn",
                  "join", "hurt", "heal", "craft", "chat", "cheat")


def mod_load_order(mods_dir):
    """Topologically sort mods by their mod.conf `depends`, as the game does."""
    depends = {}
    for mod in sorted(os.listdir(mods_dir)):
        conf = os.path.join(mods_dir, mod, "mod.conf")
        deps = []
        if os.path.isfile(conf):
            with open(conf) as fh:
                for line in fh:
                    key, _, val = line.partition("=")
                    if key.strip() == "depends":
                        deps = [d.strip() for d in val.split(",") if d.strip()]
        depends[mod] = deps

    order, done = [], set()

    def visit(mod, stack=()):
        if mod in done or mod not in depends or mod in stack:
            return
        for dep in depends[mod]:
            visit(dep, stack + (mod,))
        done.add(mod)
        order.append(mod)

    for mod in sorted(depends):
        visit(mod)
    return order


def run_hint_files(nc_root):
    """Execute every mods/*/hints.lua under a stub nc table; return raw hints."""
    mods_dir = os.path.join(nc_root, "mods")
    lua = LuaRuntime(unpack_returned_tuples=True)
    lua.execute("""
        nc = {}
        nc.hints = {}
        nc.current_mod = "?"
        nc.translate = function(s, ...) return s end
        core = {}
        function core.get_current_modname() return nc.current_mod end
        function core.get_translator() return function(x) return x end end
        function nc.register_hint(text, goal, reqs, ext)
            local info = debug.getinfo(2, "Sl")
            local h = {text = text, goal = goal, reqs = reqs,
                       mod = nc.current_mod,
                       line = info and info.currentline or 0}
            if ext then for k, v in pairs(ext) do h[k] = v end end
            nc.hints[#nc.hints + 1] = h
            return h
        end
    """)

    # nc_writing/hints.lua iterates nc.writing_glyphs; take the real list.
    api = open(os.path.join(mods_dir, "nc_writing", "api.lua")).read()
    glyphs = re.search(r"local glyphs = \{.*?\n\}\nnc\.writing_glyphs = glyphs",
                       api, re.S)
    if not glyphs:
        sys.exit("could not find the glyph table in nc_writing/api.lua")
    lua.execute(glyphs.group(0))

    loaded = []
    for mod in mod_load_order(mods_dir):
        path = os.path.join(mods_dir, mod, "hints.lua")
        if mod in SKIP_MODS or not os.path.isfile(path):
            continue
        lua.execute('nc.current_mod = "%s"' % mod)
        lua.execute(open(path).read())
        loaded.append(mod)

    return lua.globals().nc.hints, loaded


def as_text(val):
    return val.decode() if isinstance(val, bytes) else val


def as_spec(val):
    """Normalise a goal/reqs value into {"op": and|or|always, "keys": [...]}.

    nc_api_hints/register.lua's conv(): nil means always true, a string is a
    single key, a table is all-of, and a table whose first element is `true` is
    any-of.
    """
    if val is None:
        return {"op": "always", "keys": []}
    if isinstance(val, (str, bytes)):
        return {"op": "and", "keys": [as_text(val)]}
    items = [as_text(val[i]) for i in range(1, len(val) + 1)]
    if items and items[0] is True:
        return {"op": "or", "keys": items[1:]}
    return {"op": "and", "keys": items}


def slugify(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def _balanced(text, start, opener="([{", closer=")]}"):
    """Return the index just past the construct opened before `start`."""
    i, depth = start, 1
    while i < len(text) and depth:
        c = text[i]
        if c in opener:
            depth += 1
        elif c in closer:
            depth -= 1
        elif c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        i += 1
    return i


def _literal(expr, modname):
    """Resolve `"foo"` or `modname .. ":foo"`; None if it needs runtime values."""
    expr = expr.strip()
    m = re.fullmatch(r'"([^"]*)"', expr)
    if m:
        return m.group(1)
    m = re.fullmatch(r'modname\s*\.\.\s*"([^"]*)"', expr)
    if m:
        return modname + m.group(1)
    return None


def scan_crafts(nc_root):
    """Map craft labels to the items they produce.

    Hint goals name a craft by its *label* ("assemble staff"), while other
    hints' reqs name the *item* that craft yields ("nc_woodwork:staff").
    Bridging the two needs each recipe's outputs, which are its `replace =`
    targets and its `items = {...}` list.  Recipes whose label or outputs are
    computed at load time can't be read statically and are simply skipped;
    CURATED_PROVIDERS covers the ones that matter.
    """
    mods_dir = os.path.join(nc_root, "mods")
    outputs = collections.defaultdict(set)
    seen = skipped = 0
    for mod in sorted(os.listdir(mods_dir)):
        moddir = os.path.join(mods_dir, mod)
        if not os.path.isdir(moddir):
            continue
        for fn in sorted(os.listdir(moddir)):
            if not fn.endswith(".lua"):
                continue
            text = open(os.path.join(moddir, fn)).read()
            for call in re.finditer(r"nc\.register_craft\s*\(", text):
                seen += 1
                body = text[call.end():_balanced(text, call.end()) - 1]

                labels = []
                for field in ("label", "discover"):
                    m = re.search(r"\b%s\s*=\s*([^,\n]+)" % field, body)
                    if m:
                        lit = _literal(m.group(1), mod)
                        if lit:
                            labels.append(lit)
                if not labels:
                    skipped += 1
                    continue

                outs = set()
                for m in re.finditer(r"\breplace\s*=\s*([^,\n}]+)", body):
                    val = _literal(m.group(1), mod)
                    if val and val != "air":
                        outs.add(val)
                items = re.search(r"\bitems\s*=\s*\{", body)
                if items:
                    seg = body[items.end():_balanced(body, items.end(), "{", "}") - 1]
                    for m in re.finditer(r'"[^"]*"|modname\s*\.\.\s*"[^"]*"', seg):
                        val = _literal(m.group(0), mod)
                        if val and ":" in val:
                            outs.add(val)
                    for m in re.finditer(r"\bname\s*=\s*([^,\n}]+)", seg):
                        val = _literal(m.group(1), mod)
                        if val:
                            outs.add(val)
                for label in labels:
                    outputs[label] |= outs

    return ({k: sorted(v) for k, v in outputs.items() if v},
            {"recipes_seen": seen, "recipes_unreadable": skipped})


def expand_key(key):
    """Progressively strip `prefix:` segments, as nc_api_hints' expandkey does.

    `dig:nc_tree:leaves` also satisfies reqs written as `nc_tree:leaves` or
    `leaves`.  (The group/toolcap half of expandkey needs the live item
    registry; CURATED_PROVIDERS covers the keys that depend on it.)
    """
    keys, cur = [key], key
    while ":" in cur:
        cur = cur.split(":", 1)[1]
        keys.append(cur)
    return keys


def classify(key):
    if key.startswith("group:"):
        return "group"
    if key.startswith("toolcap:"):
        return "toolcap"
    if ":" in key and key.split(":", 1)[0] in EVENT_PREFIXES:
        return "event"
    if ":" in key:
        return "item"
    return "craft"


def build(nc_root):
    raw, loaded = run_hint_files(nc_root)

    nodes = []
    used_ids = collections.Counter()
    for i in range(1, len(raw) + 1):
        h = raw[i]
        mod, text = as_text(h["mod"]), as_text(h["text"])
        base = "%s/%s" % (mod, slugify(text))
        used_ids[base] += 1
        node_id = base if used_ids[base] == 1 else "%s-%d" % (base, used_ids[base])
        nodes.append({
            "id": node_id,
            "text": text,
            "mod": mod,
            "source": "mods/%s/hints.lua:%d" % (mod, h["line"]),
            "hidden": bool(h["hide"]) if h["hide"] is not None else False,
            "goal": as_spec(h["goal"]),
            "reqs": as_spec(h["reqs"]),
        })

    craft_outputs, craft_stats = scan_crafts(nc_root)

    # Index every key each hint's goal makes available: the goal keys, the
    # shortened forms expandkey would also set, and — when the goal is a craft
    # label — the items that recipe produces.
    producers = collections.defaultdict(list)

    def record(key, node_id):
        if node_id not in producers[key]:
            producers[key].append(node_id)

    for node in nodes:
        for key in node["goal"]["keys"]:
            for variant in expand_key(key):
                record(variant, node["id"])
                for item in craft_outputs.get(variant, ()):
                    for sub in expand_key(item):
                        record(sub, node["id"])

    edges = []
    unresolved = collections.defaultdict(list)
    world_reqs = collections.defaultdict(list)
    for node in nodes:
        deps, dangling = [], []
        for key in node["reqs"]["keys"]:
            found = list(producers.get(key, []))
            how = "direct"

            curated = CURATED_PROVIDERS.get(key)
            if not found and curated and not curated.get("world"):
                how = "curated"
                for alt in curated["provided_by"]:
                    for variant in [alt] + expand_key(alt):
                        found.extend(p for p in producers.get(variant, [])
                                     if p not in found)

            # An `inv:X` / `dig:X` req is stricter than a goal that merely
            # yields X, so it never matches by expansion — but the hint that
            # first gets you X is still its real prerequisite.
            if not found and ":" in key:
                for variant in expand_key(key)[1:]:
                    found.extend(p for p in producers.get(variant, [])
                                 if p not in found)
                if found:
                    how = "implied"

            if not found:
                dangling.append(key)
                if curated and curated.get("world"):
                    world_reqs[key].append(node["id"])
                else:
                    unresolved[key].append(node["id"])
                continue

            for src in found:
                if src == node["id"]:
                    continue
                edges.append({
                    "from": src,
                    "to": node["id"],
                    "via": key,
                    "req_op": node["reqs"]["op"],
                    "resolved_by": how,
                })
                if src not in deps:
                    deps.append(src)
        node["depends_on"] = deps
        node["unresolved_reqs"] = dangling
        node["req_key_kinds"] = sorted({classify(k) for k in node["reqs"]["keys"]})

    back_edges = assign_tiers(nodes)
    for e in edges:
        e["closes_cycle"] = (e["from"], e["to"]) in back_edges

    by_mod = collections.Counter(n["mod"] for n in nodes)
    return {
        "schema_version": 1,
        "meta": meta(nc_root, nodes, edges, loaded, by_mod, craft_stats),
        "key_kinds": {
            "craft": "A recipe label from nc.register_craft; discovered by "
                     "performing that craft (stored as craft:<label>).",
            "item": "An item/node name; discovered by holding, seeing, digging "
                    "or placing it.",
            "event": "A player event key: dig:, place:, inv:, look:, punch:, "
                     "hurt:, etc.",
            "group": "An item group; granted by discovering any item in that "
                     "group.",
            "toolcap": "toolcap:<diggroup>:<level>; granted by holding a tool "
                       "that digs that group at that level.",
        },
        "groups": group_glossary(nc_root),
        "curated_providers": CURATED_PROVIDERS,
        "craft_outputs": craft_outputs,
        "nodes": nodes,
        "edges": edges,
        "unresolved_req_keys": {k: sorted(v) for k, v in sorted(unresolved.items())},
        "world_provided_req_keys": {k: sorted(v) for k, v in sorted(world_reqs.items())},
        "cycle_edges": [{"from": a, "to": b} for a, b in sorted(back_edges)],
    }


def find_back_edges(nodes):
    """Edges that close a cycle, found by DFS colouring.

    The hint graph is mostly a DAG but not entirely: a few mods cross-reference
    each other (e.g. a recipe needing a tool that in turn needs that recipe's
    output).  Those edges are excluded from depth calculation and reported so a
    viewer can draw them differently.
    """
    children = collections.defaultdict(list)
    for node in nodes:
        for parent in node["depends_on"]:
            children[parent].append(node["id"])

    WHITE, GREY, BLACK = 0, 1, 2
    colour = {n["id"]: WHITE for n in nodes}
    back = set()

    for start in [n["id"] for n in nodes]:
        if colour[start] != WHITE:
            continue
        stack = [(start, iter(children[start]))]
        colour[start] = GREY
        while stack:
            parent, kids = stack[-1]
            for kid in kids:
                if colour[kid] == GREY:
                    back.add((parent, kid))
                elif colour[kid] == WHITE:
                    colour[kid] = GREY
                    stack.append((kid, iter(children[kid])))
                    break
            else:
                colour[parent] = BLACK
                stack.pop()
    return back


def assign_tiers(nodes):
    """Depth from the roots: all-of reqs wait for their slowest parent, any-of
    reqs for their fastest.  Cycle-closing edges are ignored so depth is finite."""
    back = find_back_edges(nodes)
    known = {n["id"] for n in nodes}
    tiers = {n["id"]: 0 for n in nodes}

    for _ in range(len(nodes) + 1):
        changed = False
        for node in nodes:
            parents = [p for p in node["depends_on"]
                       if p in known and (p, node["id"]) not in back]
            if not parents:
                continue
            pick = min if node["reqs"]["op"] == "or" else max
            want = pick(tiers[p] for p in parents) + 1
            if want > tiers[node["id"]]:
                tiers[node["id"]] = want
                changed = True
        if not changed:
            break

    for node in nodes:
        node["tier"] = tiers[node["id"]]
    return back


def group_glossary(nc_root):
    """NodeCore documents every item group in nc_api/item_groupdump.lua."""
    path = os.path.join(nc_root, "mods", "nc_api", "item_groupdump.lua")
    if not os.path.isfile(path):
        return {}
    text = open(path).read()
    block = re.search(r"^local groups = \{(.*?)^\}", text, re.S | re.M)
    if not block:
        return {}
    out = {}
    for name, desc in re.findall(r'(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"', block.group(1)):
        out[name] = desc.replace('\\"', '"')
    return out


def meta(nc_root, nodes, edges, loaded, by_mod, craft_stats):
    def git(*args):
        try:
            return subprocess.check_output(["git", "-C", nc_root, *args],
                                           text=True, stderr=subprocess.DEVNULL).strip()
        except Exception:
            return None

    return {
        "game": "NodeCore",
        "source_repo": git("remote", "get-url", "origin") or "https://gitlab.com/sztest/nodecore",
        "source_commit": git("rev-parse", "HEAD"),
        "source_date": git("log", "-1", "--format=%cs"),
        "generated_by": "nodecore-discovery/extract_hints.py",
        "generated_on": datetime.date.today().isoformat(),
        "hint_count": len(nodes),
        "edge_count": len(edges),
        "root_count": sum(1 for n in nodes if not n["depends_on"]),
        "max_tier": max((n["tier"] for n in nodes), default=0),
        "mods_scanned": loaded,
        "hints_per_mod": dict(by_mod.most_common()),
        "craft_scan": craft_stats,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nodecore", help="path to a NodeCore game checkout")
    ap.add_argument("-o", "--output", default="nodecore-discovery.json")
    args = ap.parse_args()

    if not os.path.isdir(os.path.join(args.nodecore, "mods", "nc_api_hints")):
        sys.exit("%s does not look like a NodeCore checkout "
                 "(no mods/nc_api_hints)" % args.nodecore)

    tree = build(args.nodecore)
    with open(args.output, "w") as fh:
        json.dump(tree, fh, indent=1, sort_keys=False)
        fh.write("\n")

    m = tree["meta"]
    print("wrote %s: %d hints, %d edges, %d roots, %d tiers"
          % (args.output, m["hint_count"], m["edge_count"], m["root_count"],
             m["max_tier"] + 1))
    if tree["unresolved_req_keys"]:
        print("unresolved prerequisite keys: %s"
              % ", ".join(sorted(tree["unresolved_req_keys"])))


if __name__ == "__main__":
    main()
