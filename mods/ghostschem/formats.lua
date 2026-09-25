-- Importing schematics written by external tools.

local gs = ghostschem

--------------------------------------------------------------------------
-- nodecore-optics-schematic v1
--------------------------------------------------------------------------
-- A sparse JSON format produced by external NodeCore build planners:
--
--   { "format": "nodecore-optics-schematic", "version": 1,
--     "name": "...", "description": "...",
--     "terrain": {"mode": "off", "top": -1, "depth": 3, "node": "..."},
--     "nodes": [ {"pos": [x, y, z], "name": "...",
--                 "param2": 3,
--                 "stack": {"name": "...", "count": 1}}, ... ] }
--
-- Three things differ from a Luanti schematic and have to be reconciled:
--
-- * It is sparse. Only listed positions hold nodes, so every unlisted cell in
--   the bounding box becomes air. The reference file is 74% air.
--
-- * Coordinates are signed and centred on the builder's own origin (the
--   reference file spans x -3..10, y -1..3, z -6..6), so they are translated
--   to a 0-based dense array. The local coordinate the builder called (0,0,0)
--   is reported back as info.origin_offset, so an anchor-on-origin paste can
--   be built on top of this later.
--
-- * Nodes may carry an item `stack`. Luanti schematics cannot store node
--   inventories at all - neither .mts nor place_schematic has anywhere to put
--   them - so stacks ride along on the node entries and are written after
--   placement by gs.apply_stacks(). place_schematic ignores the extra field:
--   read_schematic_def() reads only name, param1/prob, param2 and force_place.

local FORMAT = "nodecore-optics-schematic"
local MAX_VERSION = 1

-- Cells that no node was listed for. Node entries are never mutated after
-- import (rotation moves references, the preview and placement only read), so
-- one shared table stands in for every air cell instead of allocating
-- hundreds of thousands of identical ones.
local AIR = {name = "air", param2 = 0, prob = 255}

local function is_integer(v)
	return type(v) == "number" and v == math.floor(v)
end

function gs.import_nodecore(doc)
	if type(doc) ~= "table" then
		return nil, "not a JSON object"
	end
	if doc.format ~= FORMAT then
		return nil, string.format("unexpected format %s (expected %s)",
			tostring(doc.format), FORMAT)
	end

	local version = tonumber(doc.version)
	if not version then
		return nil, "missing version"
	end
	if version > MAX_VERSION then
		return nil, string.format(
			"version %s is newer than this importer understands (up to %d)",
			tostring(doc.version), MAX_VERSION)
	end

	if type(doc.nodes) ~= "table" or #doc.nodes == 0 then
		return nil, "schematic contains no nodes"
	end

	-- Pass one: validate and find the bounding box.
	local min, max
	for i, entry in ipairs(doc.nodes) do
		if type(entry) ~= "table" then
			return nil, string.format("node %d is not an object", i)
		end
		local p = entry.pos
		if type(p) ~= "table" or not is_integer(p[1])
				or not is_integer(p[2]) or not is_integer(p[3]) then
			return nil, string.format(
				"node %d has a malformed pos (want three integers)", i)
		end
		if type(entry.name) ~= "string" or entry.name == "" then
			return nil, string.format("node %d has no name", i)
		end
		if min then
			if p[1] < min.x then min.x = p[1] end
			if p[2] < min.y then min.y = p[2] end
			if p[3] < min.z then min.z = p[3] end
			if p[1] > max.x then max.x = p[1] end
			if p[2] > max.y then max.y = p[2] end
			if p[3] > max.z then max.z = p[3] end
		else
			min = {x = p[1], y = p[2], z = p[3]}
			max = {x = p[1], y = p[2], z = p[3]}
		end
	end

	local size = {
		x = max.x - min.x + 1,
		y = max.y - min.y + 1,
		z = max.z - min.z + 1,
	}
	local volume = size.x * size.y * size.z
	if volume > gs.MAX_IMPORT_VOLUME then
		return nil, string.format(
			"bounding box is %dx%dx%d = %d nodes, over the %d limit",
			size.x, size.y, size.z, volume, gs.MAX_IMPORT_VOLUME)
	end

	-- Pass two: fill a dense array, air everywhere nothing was listed.
	local data = {}
	for i = 1, volume do
		data[i] = AIR
	end

	local filled, duplicates = 0, 0
	local names = {}
	for _, entry in ipairs(doc.nodes) do
		local p = entry.pos
		local index = gs.index(size,
			p[1] - min.x, p[2] - min.y, p[3] - min.z)

		if data[index] ~= AIR then
			duplicates = duplicates + 1
		else
			filled = filled + 1
		end

		local node = {
			name = entry.name,
			param2 = is_integer(entry.param2) and entry.param2 or 0,
			prob = 255,
		}

		if type(entry.stack) == "table" and type(entry.stack.name) == "string"
				and entry.stack.name ~= "" then
			local count = tonumber(entry.stack.count) or 1
			node.stack = {
				name = entry.stack.name,
				count = math.max(1, math.floor(count)),
			}
		end

		data[index] = node
		names[entry.name] = (names[entry.name] or 0) + 1
	end

	-- Counted from the final array rather than while reading, so a stack on
	-- an entry that a later duplicate replaced is not counted.
	local stacks = gs.count_stacks({size = size, data = data})

	local info = {
		source_format = FORMAT,
		source_version = version,
		name = doc.name,
		description = doc.description,
		filled = filled,
		duplicates = duplicates,
		stacks = stacks,
		names = names,
		-- Where the builder's own (0,0,0) sits inside the array.
		origin_offset = {x = -min.x, y = -min.y, z = -min.z},
	}

	-- Terrain generation is the builder's own concept and its modes are not
	-- documented anywhere we can check, so rather than guess at semantics and
	-- produce a preview that lies, anything other than "off" is skipped and
	-- reported.
	local terrain = doc.terrain
	if type(terrain) == "table" then
		info.terrain = terrain
		local mode = terrain.mode
		if mode ~= nil and mode ~= "off" then
			info.terrain_skipped = tostring(mode)
		end
	end

	return {size = size, data = data}, info
end

--------------------------------------------------------------------------
-- Dispatch
--------------------------------------------------------------------------

gs.MAX_IMPORT_VOLUME = tonumber(
	core.settings:get("ghostschem_max_import_volume")) or 512 * 1024

-- Importers keyed by the format string they handle, so another external tool
-- can be added without touching the loading code.
gs.json_importers = {
	[FORMAT] = gs.import_nodecore,
}

function gs.import_json(text)
	if type(text) ~= "string" or text == "" then
		return nil, "empty file"
	end

	local doc, err = core.parse_json(text, nil, true)
	if doc == nil then
		return nil, "invalid JSON: " .. tostring(err)
	end
	if type(doc) ~= "table" then
		return nil, "JSON root is not an object"
	end

	local importer = gs.json_importers[doc.format]
	if not importer then
		local known = {}
		for name in pairs(gs.json_importers) do
			known[#known + 1] = name
		end
		return nil, string.format("unknown format %s (known: %s)",
			tostring(doc.format), table.concat(known, ", "))
	end

	return importer(doc)
end

--------------------------------------------------------------------------
-- Reporting
--------------------------------------------------------------------------

function gs.describe_import(schem, info)
	local lines = {}
	local s = schem.size
	lines[#lines + 1] = string.format("%s: %dx%dx%d, %d nodes placed, %d air",
		info.name and ('"' .. info.name .. '"') or info.source_format,
		s.x, s.y, s.z, info.filled,
		s.x * s.y * s.z - info.filled)

	if info.duplicates > 0 then
		lines[#lines + 1] = string.format(
			"%d duplicate position(s); the last entry won", info.duplicates)
	end
	if info.stacks > 0 then
		lines[#lines + 1] = string.format(
			"%d node(s) carry item stacks (written after placement)",
			info.stacks)
	end
	if info.terrain_skipped then
		lines[#lines + 1] = string.format(
			"terrain mode '%s' skipped; only the listed nodes were imported",
			info.terrain_skipped)
	end

	-- Anything the running game does not have cannot be placed, so say which.
	local missing = {}
	for name in pairs(info.names) do
		if not core.registered_nodes[name] then
			missing[#missing + 1] = name
		end
	end
	if #missing > 0 then
		table.sort(missing)
		lines[#lines + 1] = string.format(
			"%d unknown node type(s) in this game: %s",
			#missing, table.concat(missing, ", "))
	end

	return table.concat(lines, "\n  ")
end

--------------------------------------------------------------------------
-- Item stacks
--------------------------------------------------------------------------

-- A node's inventory lists are created by its own on_construct, so they only
-- exist once the node is in the map. That makes the list name discoverable at
-- runtime rather than something we have to guess per game.
function gs.put_stack(pos, stack)
	local item = ItemStack(stack.name .. " " .. stack.count)
	if not item:is_known() then
		return false, "unknown item " .. stack.name
	end

	local inv = core.get_meta(pos):get_inventory()
	local lists = inv:get_lists()

	local candidates = {}
	for name in pairs(lists) do
		candidates[#candidates + 1] = name
	end

	if #candidates == 0 then
		return false, "node has no inventory to put it in"
	end

	local target
	if #candidates == 1 then
		target = candidates[1]
	elseif lists.main then
		target = "main"
	else
		table.sort(candidates)
		return false, "ambiguous inventory (lists: " ..
			table.concat(candidates, ", ") .. ")"
	end

	if not inv:room_for_item(target, item) then
		return false, "no room in list '" .. target .. "'"
	end

	inv:add_item(target, item)
	return true
end

-- Schematic placement goes through a VoxelManip: Schematic::placeOnMap() blits
-- the data and dispatches a map edit event, but it never runs on_construct.
-- A container placed by a schematic therefore has no inventory lists at all -
-- they are created by the node's own on_construct - so writing a stack has to
-- construct the node first. core.set_node() does run the callbacks, so the
-- stack-carrying nodes (there are usually a handful) are re-set individually.
local function construct_node(pos, node)
	local existing = core.get_node(pos)
	if existing.name ~= node.name then
		-- The schematic did not actually place this node: force_placement was
		-- off and something was already here. Do not force it now.
		return false, "node was not placed (" .. existing.name .. " is there)"
	end
	core.set_node(pos, {name = node.name, param2 = node.param2 or 0})
	return true
end

function gs.apply_stacks(origin, schem)
	local applied, failed = 0, {}
	local size = schem.size
	for z = 0, size.z - 1 do
		for y = 0, size.y - 1 do
			for x = 0, size.x - 1 do
				local node = gs.get(schem, x, y, z)
				if node and node.stack then
					local pos = vector.new(
						origin.x + x, origin.y + y, origin.z + z)

					local ok, why = construct_node(pos, node)
					if ok then
						ok, why = gs.put_stack(pos, node.stack)
					end

					if ok then
						applied = applied + 1
					else
						failed[#failed + 1] = {
							pos = pos,
							node = node.name,
							item = node.stack.name,
							why = why,
						}
					end
				end
			end
		end
	end
	return applied, failed
end

function gs.count_stacks(schem)
	local n = 0
	local size = schem.size
	for z = 0, size.z - 1 do
		for y = 0, size.y - 1 do
			for x = 0, size.x - 1 do
				local node = gs.get(schem, x, y, z)
				if node and node.stack then
					n = n + 1
				end
			end
		end
	end
	return n
end

-- Stacks are the one part of an import that placement can silently drop, so
-- say what happened rather than reporting a clean "Placed."
function gs.describe_stack_result(counts)
	local applied = counts.stacks_applied
	if not applied then
		return ""
	end

	local failed = counts.stacks_failed or {}
	local msg = string.format(" %d item stack(s) written.", applied)
	if #failed > 0 then
		local why = {}
		local seen = {}
		for _, f in ipairs(failed) do
			if not seen[f.why] then
				seen[f.why] = true
				why[#why + 1] = f.why
			end
		end
		msg = msg .. string.format(" %d could NOT be written (%s).",
			#failed, table.concat(why, "; "))
	end
	return msg
end
