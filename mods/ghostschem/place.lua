-- Committing a preview to the map, and undoing it.

local gs = ghostschem

local MAX_UNDO = tonumber(core.settings:get("ghostschem_undo_depth")) or 10

gs.clipboard = {}   -- [player_name] = clipboard entry (see below)
gs.undo_stack = {}  -- [player_name] = { snapshot, ... }

--------------------------------------------------------------------------
-- Snapshots
--------------------------------------------------------------------------
-- We snapshot with a VoxelManip rather than core.create_schematic(), because
-- create_schematic() requires a filename (l_mapgen.cpp does luaL_checkstring
-- on argument 4) and would mean a disk write per paste. A VoxelManip capture
-- is exact, in memory, and records param2 as well.
--
-- Caveat worth knowing: get_data() returns content IDs, which are only stable
-- within a single server run, and VoxelManip does not carry node metadata or
-- inventories. So this is an in-session undo. That matches what it is undoing:
-- place_schematic() does not write metadata either.

local function snapshot(p1, p2)
	local vm = core.get_voxel_manip()
	local emin, emax = vm:read_from_map(p1, p2)
	return {
		p1 = vector.copy(p1),
		p2 = vector.copy(p2),
		emin = emin,
		emax = emax,
		data = vm:get_data(),
		param2 = vm:get_param2_data(),
	}
end

local function restore(snap)
	local vm = core.get_voxel_manip()
	local emin, emax = vm:read_from_map(snap.p1, snap.p2)
	if not vector.equals(emin, snap.emin) or not vector.equals(emax, snap.emax) then
		return false, "the emerged area changed shape since the snapshot"
	end
	vm:set_data(snap.data)
	vm:set_param2_data(snap.param2)
	vm:write_to_map(true)
	return true
end

local function push_undo(player_name, snap)
	local stack = gs.undo_stack[player_name]
	if not stack then
		stack = {}
		gs.undo_stack[player_name] = stack
	end
	stack[#stack + 1] = snap
	while #stack > MAX_UNDO do
		table.remove(stack, 1)
	end
end

--------------------------------------------------------------------------
-- Clipboard
--------------------------------------------------------------------------

-- A clipboard entry keeps the source region alongside the schematic, because
-- exporting a real .mts needs map coordinates: core.create_schematic() reads
-- from the map, it cannot serialize a table.
--
--   { schem = <schematic table>, p1 = , p2 = , origin = "copy" | "file" }

function gs.copy(player_name, p1, p2)
	p1, p2 = vector.sort(p1, p2)
	local volume = (p2.x - p1.x + 1) * (p2.y - p1.y + 1) * (p2.z - p1.z + 1)
	if volume > 512 * 1024 then
		return nil, "region too large (" .. volume .. " nodes)"
	end
	local schem = gs.capture(p1, p2)
	gs.clipboard[player_name] = {
		schem = schem,
		p1 = p1,
		p2 = p2,
		origin = "copy",
	}
	return schem
end

function gs.get_clipboard(player_name)
	local entry = gs.clipboard[player_name]
	return entry and entry.schem, entry
end

--------------------------------------------------------------------------
-- Commit
--------------------------------------------------------------------------

function gs.commit(player_name)
	local preview = gs.get_preview(player_name)
	if not preview then
		return false, "no active preview"
	end

	local p1, p2 = preview:bounds()
	core.load_area(p1, p2)

	local snap = snapshot(p1, p2)

	-- Pass the *unrotated* schematic plus the rotation string, so the engine
	-- applies its own rotation. The preview used the same mapping derived from
	-- blitToVManip(), so what lands is what was shown.
	local ok = core.place_schematic(
		preview.origin,
		preview.schem,
		preview.rotation,
		nil,          -- replacements
		preview.force -- force_placement
	)
	if ok == nil then
		return false, "place_schematic failed to load the schematic"
	end

	push_undo(player_name, snap)
	local counts = preview.counts
	gs.hide(player_name)
	return true, counts
end

--------------------------------------------------------------------------
-- Undo
--------------------------------------------------------------------------

function gs.undo(player_name)
	local stack = gs.undo_stack[player_name]
	if not stack or #stack == 0 then
		return false, "nothing to undo"
	end
	local snap = table.remove(stack)
	local ok, err = restore(snap)
	if not ok then
		-- Put it back so the player can retry once the area is loaded again.
		stack[#stack + 1] = snap
		return false, err
	end
	return true, snap
end

function gs.undo_depth(player_name)
	local stack = gs.undo_stack[player_name]
	return stack and #stack or 0
end

core.register_on_leaveplayer(function(player)
	local name = player:get_player_name()
	gs.undo_stack[name] = nil
	gs.clipboard[name] = nil
end)

--------------------------------------------------------------------------
-- Files
--------------------------------------------------------------------------

local function schem_dir()
	local dir = core.get_worldpath() .. DIR_DELIM .. "schems"
	core.mkdir(dir)
	return dir
end

local function safe_name(name)
	if type(name) ~= "string" or not name:match("^[%w_%-]+$") then
		return nil
	end
	return name
end

-- Export as a real .mts, so the result is readable by WorldEdit, by mapgen
-- decorations, and by core.place_schematic anywhere else.
function gs.save_file(player_name, name)
	name = safe_name(name)
	if not name then
		return false, "name may only contain letters, digits, _ and -"
	end

	local entry = gs.clipboard[player_name]
	if not entry then
		return false, "clipboard is empty"
	end
	if not entry.p1 then
		return false, "this clipboard came from a file, so there is no map " ..
			"region to export; paste it and copy it again to re-export"
	end

	local path = schem_dir() .. DIR_DELIM .. name .. ".mts"
	core.load_area(entry.p1, entry.p2)
	if core.create_schematic(entry.p1, entry.p2, nil, path) == nil then
		return false, "create_schematic failed (is the area still loaded?)"
	end
	return true, path
end

function gs.load_file(player_name, name)
	name = safe_name(name)
	if not name then
		return false, "name may only contain letters, digits, _ and -"
	end

	local path = schem_dir() .. DIR_DELIM .. name .. ".mts"
	-- Read straight into a table. Everything downstream then places from the
	-- table, which sidesteps place_schematic's permanent per-filename cache.
	local schem, err = gs.load(path)
	if not schem then
		return false, "no such schematic: " .. name .. " (" .. tostring(err) .. ")"
	end

	gs.clipboard[player_name] = {
		schem = schem,
		origin = "file",
		name = name,
	}
	return true, schem
end

function gs.list_files()
	local out = {}
	for _, entry in ipairs(core.get_dir_list(schem_dir(), false) or {}) do
		local base = entry:match("^(.+)%\.mts$")
		if base then
			out[#out + 1] = base
		end
	end
	table.sort(out)
	return out
end
