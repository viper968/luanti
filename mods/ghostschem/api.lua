-- Schematic data model.
--
-- Everything here operates on the plain table form that core.read_schematic
-- returns:
--
--   { size = {x=, y=, z=},
--     data = { {name=, prob=, param2=, force_place=}, ... },
--     yslice_prob = { {ypos=, prob=}, ... } }
--
-- We deliberately keep schematics as tables rather than filenames, because
-- core.place_schematic() permanently caches anything loaded from a file (see
-- doc/lua_api.md: "The only way to load the file anew is to restart the
-- server"). Passing tables sidesteps that cache entirely, which matters a lot
-- for an edit-preview-place workflow.

local gs = ghostschem

--------------------------------------------------------------------------
-- Array layout
--------------------------------------------------------------------------
-- This is verified against the engine rather than assumed, because getting it
-- wrong produces a preview that silently lies about where nodes land.
--
--   src/mapgen/mg_schematic.cpp, Schematic::blitToVManip():
--       xstride = 1;  ystride = size.X;  zstride = size.X * size.Y;
--   src/script/lua_api/l_mapgen.cpp, l_read_schematic():
--       data[i + 1] = schemdata[i]
--
-- So the array is X-fastest, then Y, then Z. Note this is *not* the
-- z-major order most voxel formats use.

function gs.index(size, x, y, z)
	return 1 + x + y * size.x + z * size.x * size.y
end

function gs.get(schem, x, y, z)
	return schem.data[gs.index(schem.size, x, y, z)]
end

--------------------------------------------------------------------------
-- Loading
--------------------------------------------------------------------------

-- Accepts anything core.read_schematic accepts: a filename, a schematic table,
-- or a registered schematic name.
function gs.load(spec)
	local ok, schem = pcall(core.read_schematic, spec, {
		write_yslice_prob = "all",
	})
	if not ok then
		return nil, tostring(schem)
	end
	if type(schem) ~= "table" or type(schem.size) ~= "table" then
		return nil, "not a readable schematic"
	end
	if schem.size.x < 1 or schem.size.y < 1 or schem.size.z < 1 then
		return nil, "schematic has zero volume"
	end
	return schem
end

-- Capture a region of the map as a schematic table, without touching disk.
-- core.create_schematic() requires a filename (l_mapgen.cpp uses
-- luaL_checkstring on argument 4), so we build the table ourselves.
function gs.capture(p1, p2)
	p1, p2 = vector.sort(p1, p2)
	core.load_area(p1, p2)

	local size = {
		x = p2.x - p1.x + 1,
		y = p2.y - p1.y + 1,
		z = p2.z - p1.z + 1,
	}
	local data = {}
	local pos = vector.new()
	for z = 0, size.z - 1 do
		for y = 0, size.y - 1 do
			for x = 0, size.x - 1 do
				pos.x, pos.y, pos.z = p1.x + x, p1.y + y, p1.z + z
				local node = core.get_node(pos)
				data[gs.index(size, x, y, z)] = {
					name = node.name,
					param2 = node.param2,
					prob = 255,
				}
			end
		end
	end
	return {size = size, data = data}
end

--------------------------------------------------------------------------
-- Rotation
--------------------------------------------------------------------------
-- These mappings are derived from the i_start / i_step_x / i_step_z values in
-- blitToVManip(), so a rotated preview lines up exactly with what
-- core.place_schematic(pos, schem, rotation) will actually write:
--
--   "90"  (x, z) -> (z,          sx - 1 - x)   size -> (sz, sy, sx)
--   "180" (x, z) -> (sx - 1 - x, sz - 1 - z)   size unchanged
--   "270" (x, z) -> (sz - 1 - z, x)            size -> (sz, sy, sx)
--
-- param2 is left alone: the engine rotates it itself at placement time via
-- MapNode::rotateAlongYAxis(), and the cube-based ghost visual cannot show
-- facedir anyway. A `visual = "node"` ghost (see README, tier 1) would need
-- param2 rotated here too.

gs.ROTATIONS = {"0", "90", "180", "270"}

local ROT_STEPS = {["0"] = 0, ["90"] = 1, ["180"] = 2, ["270"] = 3}

function gs.is_rotation(rot)
	return ROT_STEPS[rot] ~= nil
end

function gs.next_rotation(rot)
	local steps = (ROT_STEPS[rot] or 0) + 1
	return gs.ROTATIONS[(steps % 4) + 1]
end

-- Size the schematic occupies on the map once rotated.
function gs.rotated_size(size, rot)
	local steps = ROT_STEPS[rot] or 0
	if steps == 1 or steps == 3 then
		return {x = size.z, y = size.y, z = size.x}
	end
	return {x = size.x, y = size.y, z = size.z}
end

function gs.rotate(schem, rot)
	local steps = ROT_STEPS[rot] or 0
	if steps == 0 then
		return schem
	end

	local size = schem.size
	local sx, sy, sz = size.x, size.y, size.z
	local nsize = gs.rotated_size(size, rot)
	local out = {size = nsize, yslice_prob = schem.yslice_prob, data = {}}

	for z = 0, sz - 1 do
		for y = 0, sy - 1 do
			for x = 0, sx - 1 do
				local nx, nz
				if steps == 1 then
					nx, nz = z, sx - 1 - x
				elseif steps == 2 then
					nx, nz = sx - 1 - x, sz - 1 - z
				else
					nx, nz = sz - 1 - z, x
				end
				out.data[gs.index(nsize, nx, y, nz)] =
					schem.data[gs.index(size, x, y, z)]
			end
		end
	end
	return out
end

--------------------------------------------------------------------------
-- Visibility / occlusion culling
--------------------------------------------------------------------------
-- A ghost is one entity per node, so interior nodes are by far the biggest
-- cost and the easiest win: a solid 16x16x16 schematic is 4096 nodes but only
-- 1352 of them are on the shell.

local opaque_cache = {}

function gs.is_opaque(name)
	local cached = opaque_cache[name]
	if cached ~= nil then
		return cached
	end
	local def = core.registered_nodes[name]
	local result = false
	if def then
		local drawtype = def.drawtype or "normal"
		local alpha = def.use_texture_alpha
		result = drawtype == "normal"
			and (alpha == nil or alpha == false or alpha == "opaque")
	end
	opaque_cache[name] = result
	return result
end

function gs.is_empty(name)
	return name == nil or name == "air" or name == "ignore"
end

local NEIGHBOURS = {
	{1, 0, 0}, {-1, 0, 0},
	{0, 1, 0}, {0, -1, 0},
	{0, 0, 1}, {0, 0, -1},
}

-- True when this node is opaque and every one of its six neighbours is an
-- opaque node *inside the schematic*. Boundary nodes are never enclosed,
-- since you can see them from outside.
local function is_enclosed(schem, x, y, z, node)
	if not gs.is_opaque(node.name) then
		return false
	end
	local size = schem.size
	for i = 1, 6 do
		local d = NEIGHBOURS[i]
		local nx, ny, nz = x + d[1], y + d[2], z + d[3]
		if nx < 0 or ny < 0 or nz < 0
				or nx >= size.x or ny >= size.y or nz >= size.z then
			return false
		end
		local n = gs.get(schem, nx, ny, nz)
		if not (n and gs.is_opaque(n.name)) then
			return false
		end
	end
	return true
end

-- Calls fn(x, y, z, node) for every node that would actually be visible.
-- Returns shown, total.
function gs.each_visible(schem, fn)
	local size = schem.size
	local shown, total = 0, 0
	for z = 0, size.z - 1 do
		for y = 0, size.y - 1 do
			for x = 0, size.x - 1 do
				local node = gs.get(schem, x, y, z)
				local name = node and node.name
				if not gs.is_empty(name) and (node.prob == nil or node.prob > 0) then
					total = total + 1
					if not is_enclosed(schem, x, y, z, node) then
						shown = shown + 1
						fn(x, y, z, node)
					end
				end
			end
		end
	end
	return shown, total
end

-- Count without spawning anything, for budget checks before building.
function gs.count_visible(schem)
	return gs.each_visible(schem, function() end)
end
