-- Chat interface.

local gs = ghostschem

core.register_privilege("ghostschem", {
	description = "Can use ghost schematic previews and place schematics",
	give_to_singleplayer = true,
})

local subcommands = {}

local function reply(name, msg)
	core.chat_send_player(name, msg)
end

--------------------------------------------------------------------------
-- Selection
--------------------------------------------------------------------------

subcommands.pos1 = {
	help = "set corner 1 to your position",
	run = function(name, player)
		local pos = vector.round(player:get_pos())
		gs.set_corner(name, "p1", pos)
		return true, "Corner 1 set to " .. core.pos_to_string(pos)
	end,
}

subcommands.pos2 = {
	help = "set corner 2 to your position",
	run = function(name, player)
		local pos = vector.round(player:get_pos())
		gs.set_corner(name, "p2", pos)
		return true, "Corner 2 set to " .. core.pos_to_string(pos)
	end,
}

subcommands.deselect = {
	help = "clear the current selection",
	run = function(name)
		gs.clear_selection(name)
		return true, "Selection cleared."
	end,
}

--------------------------------------------------------------------------
-- Clipboard
--------------------------------------------------------------------------

subcommands.copy = {
	help = "copy the selected region into the clipboard",
	run = function(name)
		local p1, p2 = gs.get_region(name)
		if not p1 then
			return false, p2
		end
		local schem, err = gs.copy(name, p1, p2)
		if not schem then
			return false, err
		end
		local s = schem.size
		return true, string.format("Copied %dx%dx%d (%d nodes).",
			s.x, s.y, s.z, s.x * s.y * s.z)
	end,
}

--------------------------------------------------------------------------
-- Preview
--------------------------------------------------------------------------

subcommands.show = {
	help = "show the ghost preview at your position",
	run = function(name, player)
		local schem = gs.get_clipboard(name)
		if not schem then
			return false, "clipboard is empty; use /gs copy or /gs load first"
		end
		local preview = gs.show(name, schem, player:get_pos(), "0", true)
		return true, preview:summary()
	end,
}

subcommands.hide = {
	help = "hide the ghost preview",
	run = function(name)
		if gs.hide(name) then
			return true, "Preview hidden."
		end
		return false, "no active preview"
	end,
}

subcommands.move = {
	help = "move the ghost preview to your position",
	run = function(name, player)
		local preview = gs.get_preview(name)
		if not preview then
			return false, "no active preview"
		end
		preview.origin = vector.round(player:get_pos())
		preview:refresh()
		return true, preview:summary()
	end,
}

subcommands.rotate = {
	params = "[0|90|180|270]",
	help = "rotate the preview (no argument steps by 90)",
	run = function(name, _, arg)
		local preview = gs.get_preview(name)
		if not preview then
			return false, "no active preview"
		end
		if arg == "" then
			preview.rotation = gs.next_rotation(preview.rotation)
		elseif gs.is_rotation(arg) then
			preview.rotation = arg
		else
			return false, "rotation must be 0, 90, 180 or 270"
		end
		preview:refresh()
		return true, preview:summary()
	end,
}

subcommands.force = {
	params = "[on|off]",
	help = "whether placement overwrites existing nodes (default on)",
	run = function(name, _, arg)
		local preview = gs.get_preview(name)
		if not preview then
			return false, "no active preview"
		end
		if arg == "on" then
			preview.force = true
		elseif arg == "off" then
			preview.force = false
		elseif arg == "" then
			preview.force = not preview.force
		else
			return false, "expected 'on' or 'off'"
		end
		preview:refresh()
		return true, preview:summary()
	end,
}

subcommands.status = {
	help = "describe the current preview",
	run = function(name)
		local preview = gs.get_preview(name)
		if not preview then
			return false, "no active preview"
		end
		return true, preview:summary()
	end,
}

--------------------------------------------------------------------------
-- Commit and undo
--------------------------------------------------------------------------

subcommands.place = {
	help = "commit the ghost preview to the map",
	run = function(name)
		local ok, result = gs.commit(name)
		if not ok then
			return false, result
		end
		local msg = "Placed."
		if result.overwrite > 0 then
			msg = msg .. " " .. result.overwrite .. " node(s) overwritten."
		end
		if result.skipped > 0 then
			msg = msg .. " " .. result.skipped .. " node(s) skipped."
		end
		return true, msg .. " /gs undo to revert."
	end,
}

subcommands.undo = {
	help = "undo the last placement",
	run = function(name)
		local ok, result = gs.undo(name)
		if not ok then
			return false, result
		end
		return true, string.format("Undone %s .. %s (%d left).",
			core.pos_to_string(result.p1), core.pos_to_string(result.p2),
			gs.undo_depth(name))
	end,
}

--------------------------------------------------------------------------
-- Files
--------------------------------------------------------------------------

subcommands.save = {
	params = "<name>",
	help = "export the clipboard to worldpath/schems/<name>.mts",
	run = function(name, _, arg)
		local ok, result = gs.save_file(name, arg)
		if not ok then
			return false, result
		end
		return true, "Saved to " .. result
	end,
}

subcommands.load = {
	params = "<name>",
	help = "load worldpath/schems/<name>.mts into the clipboard",
	run = function(name, _, arg)
		local ok, result = gs.load_file(name, arg)
		if not ok then
			return false, result
		end
		local s = result.size
		return true, string.format("Loaded %dx%dx%d. Use /gs show.", s.x, s.y, s.z)
	end,
}

subcommands.list = {
	help = "list saved schematics",
	run = function()
		local names = gs.list_files()
		if #names == 0 then
			return true, "No saved schematics."
		end
		return true, "Saved: " .. table.concat(names, ", ")
	end,
}

--------------------------------------------------------------------------
-- Dispatch
--------------------------------------------------------------------------

subcommands.selftest = {
	help = "verify the preview against what place_schematic really writes",
	run = function(name)
		if not gs.selftest then
			return false, "set ghostschem_selftest = true and restart to enable"
		end
		return gs.selftest(name)
	end,
}

subcommands.help = {
	help = "show this help",
	run = function()
		local keys = {}
		for key in pairs(subcommands) do
			keys[#keys + 1] = key
		end
		table.sort(keys)
		local lines = {"Ghost schematics:"}
		for _, key in ipairs(keys) do
			local sub = subcommands[key]
			lines[#lines + 1] = string.format("  /gs %s %s - %s",
				key, sub.params or "", sub.help)
		end
		return true, table.concat(lines, "\n")
	end,
}

core.register_chatcommand("gs", {
	params = "<subcommand> [args]",
	description = "Ghost schematic previews (/gs help)",
	privs = {ghostschem = true},
	func = function(name, param)
		local player = core.get_player_by_name(name)
		if not player then
			return false, "you must be in-game"
		end

		local key, arg = param:match("^(%S*)%s*(.-)%s*$")
		if key == "" then
			key = "help"
		end

		local sub = subcommands[key]
		if not sub then
			return false, "unknown subcommand '" .. key .. "'; try /gs help"
		end
		return sub.run(name, player, arg or "")
	end,
})

-- Convenience: hand out the tools.
core.register_chatcommand("gswand", {
	description = "Give yourself the ghost schematic wand and placer",
	privs = {ghostschem = true},
	func = function(name)
		local player = core.get_player_by_name(name)
		if not player then
			return false, "you must be in-game"
		end
		local inv = player:get_inventory()
		inv:add_item("main", "ghostschem:wand")
		inv:add_item("main", "ghostschem:placer")
		return true, "Given the ghost wand and placer."
	end,
})
