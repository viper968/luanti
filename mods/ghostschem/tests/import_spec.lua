-- Importer tests. These feed gs.import_nodecore() decoded tables directly, so
-- they exercise the transform without needing a JSON parser or a server.

core = {
	settings = {get = function() return nil end},
	registered_nodes = {["nc_terrain:stone"] = {drawtype = "normal"}},
}
vector = {}
ghostschem = {}
dofile("/home/user/luanti/mods/ghostschem/api.lua")
dofile("/home/user/luanti/mods/ghostschem/formats.lua")
local gs = ghostschem

local failed, checks = 0, 0
local function check(label, got, want)
	checks = checks + 1
	local ok = got == want
	if not ok then failed = failed + 1 end
	print(string.format("%-52s %s%s", label, ok and "ok" or "FAIL",
		ok and "" or string.format("  (got %s, want %s)",
			tostring(got), tostring(want))))
end

local function doc(nodes, extra)
	local d = {format = "nodecore-optics-schematic", version = 1, nodes = nodes}
	for k, v in pairs(extra or {}) do d[k] = v end
	return d
end

-- 1. Single node.
local schem, info = gs.import_nodecore(doc({
	{pos = {0, 0, 0}, name = "nc_terrain:stone"},
}))
check("single node: size x", schem.size.x, 1)
check("single node: size y", schem.size.y, 1)
check("single node: size z", schem.size.z, 1)
check("single node: name", gs.get(schem, 0, 0, 0).name, "nc_terrain:stone")

-- 2. Signed coordinates are translated to a 0-based array.
schem, info = gs.import_nodecore(doc({
	{pos = {-3, -1, -6}, name = "a"},
	{pos = {10, 3, 6}, name = "b"},
}))
check("signed bbox: size x", schem.size.x, 14)
check("signed bbox: size y", schem.size.y, 5)
check("signed bbox: size z", schem.size.z, 13)
check("signed bbox: min corner node", gs.get(schem, 0, 0, 0).name, "a")
check("signed bbox: max corner node", gs.get(schem, 13, 4, 12).name, "b")
check("signed bbox: origin_offset x", info.origin_offset.x, 3)
check("signed bbox: origin_offset y", info.origin_offset.y, 1)
check("signed bbox: origin_offset z", info.origin_offset.z, 6)

-- The builder's own (0,0,0) must land where origin_offset says it does.
schem = gs.import_nodecore(doc({
	{pos = {-3, -1, -6}, name = "a"},
	{pos = {0, 0, 0}, name = "origin"},
	{pos = {10, 3, 6}, name = "b"},
}))
check("builder origin lands at origin_offset",
	gs.get(schem, 3, 1, 6).name, "origin")

-- 3. Unlisted cells become air.
schem, info = gs.import_nodecore(doc({
	{pos = {0, 0, 0}, name = "a"},
	{pos = {2, 0, 0}, name = "b"},
}))
check("sparse: gap is air", gs.get(schem, 1, 0, 0).name, "air")
check("sparse: filled count", info.filled, 2)
check("sparse: air is never previewed", select(2, gs.each_visible(schem,
	function() end)), 2)

-- 4. param2 and stacks.
schem, info = gs.import_nodecore(doc({
	{pos = {0, 0, 0}, name = "nc_optics:prism", param2 = 14},
	{pos = {1, 0, 0}, name = "nc_woodwork:form",
		stack = {name = "nc_tree:stick", count = 100}},
	{pos = {2, 0, 0}, name = "plain"},
}))
check("param2 preserved", gs.get(schem, 0, 0, 0).param2, 14)
check("param2 defaults to 0", gs.get(schem, 2, 0, 0).param2, 0)
check("stack name", gs.get(schem, 1, 0, 0).stack.name, "nc_tree:stick")
check("stack count", gs.get(schem, 1, 0, 0).stack.count, 100)
check("stack counted in info", info.stacks, 1)
check("count_stacks agrees", gs.count_stacks(schem), 1)
check("nodes without stacks have none",
	tostring(gs.get(schem, 0, 0, 0).stack), "nil")

-- 5. Rotation must carry stacks with the node. A stack left behind at the
--    pre-rotation coordinate would silently load the wrong container.
local rotated = gs.rotate(schem, "90")
check("rotation preserves stack count", gs.count_stacks(rotated), 1)
-- "90": (x, z) -> (z, sx - 1 - x), so x=1 of a 3-wide schematic -> z = 1
check("rotation moves the stack with its node",
	gs.get(rotated, 0, 0, 1).stack.name, "nc_tree:stick")
check("rotation keeps stack on the right node",
	gs.get(rotated, 0, 0, 1).name, "nc_woodwork:form")

-- 6. Duplicate positions: last wins, and it is reported rather than hidden.
schem, info = gs.import_nodecore(doc({
	{pos = {0, 0, 0}, name = "first"},
	{pos = {0, 0, 0}, name = "second"},
}))
check("duplicate: last entry wins", gs.get(schem, 0, 0, 0).name, "second")
check("duplicate: counted", info.duplicates, 1)
check("duplicate: not double counted as filled", info.filled, 1)

-- 7. Terrain modes other than "off" are skipped and reported, never guessed.
schem, info = gs.import_nodecore(doc({{pos = {0, 0, 0}, name = "a"}},
	{terrain = {mode = "off", top = -1, depth = 3, node = "s"}}))
check("terrain off: nothing reported", tostring(info.terrain_skipped), "nil")

schem, info = gs.import_nodecore(doc({{pos = {0, 0, 0}, name = "a"}},
	{terrain = {mode = "flat", top = -1, depth = 3, node = "s"}}))
check("terrain flat: reported as skipped", info.terrain_skipped, "flat")

-- 8. Unknown node types are surfaced, since they cannot be placed.
schem, info = gs.import_nodecore(doc({
	{pos = {0, 0, 0}, name = "nc_terrain:stone"},
	{pos = {1, 0, 0}, name = "nosuchmod:nosuchnode"},
}))
local desc = gs.describe_import(schem, info)
check("unknown node types are reported",
	desc:find("nosuchmod:nosuchnode", 1, true) ~= nil, true)
check("registered node types are not reported as missing",
	desc:find("nc_terrain:stone", 1, true), nil)

-- 9. Rejections.
local function rejects(label, d)
	local got, err = gs.import_nodecore(d)
	checks = checks + 1
	local ok = got == nil and type(err) == "string"
	if not ok then failed = failed + 1 end
	print(string.format("%-52s %s%s", label, ok and "ok" or "FAIL",
		ok and ("  (" .. tostring(err) .. ")") or "  (accepted bad input!)"))
end

rejects("reject: not a table", "nope")
rejects("reject: wrong format", {format = "other", version = 1, nodes = {}})
rejects("reject: future version",
	doc({{pos = {0, 0, 0}, name = "a"}}, {version = 99}))
rejects("reject: no nodes", doc({}))
rejects("reject: malformed pos", doc({{pos = {0, 0}, name = "a"}}))
rejects("reject: non-integer pos", doc({{pos = {0, 0.5, 0}, name = "a"}}))
rejects("reject: missing name", doc({{pos = {0, 0, 0}}}))
rejects("reject: oversized bounding box", doc({
	{pos = {0, 0, 0}, name = "a"},
	{pos = {5000, 5000, 5000}, name = "b"},
}))

print()
print(failed == 0
	and string.format("all %d import checks passed", checks)
	or string.format("%d of %d import checks FAILED", failed, checks))
os.exit(failed == 0 and 0 or 1)
