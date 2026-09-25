-- Region selection and the ghost placer tool.

local gs = ghostschem

gs.selection = {}  -- [player_name] = {p1 = , p2 = , markers = {}}

--------------------------------------------------------------------------
-- Selection markers
--------------------------------------------------------------------------

local function clear_markers(sel)
	for i = 1, #(sel.markers or {}) do
		local obj = sel.markers[i]
		if obj and obj:get_pos() then
			obj:remove()
		end
	end
	sel.markers = {}
end

local function draw_markers(sel)
	clear_markers(sel)
	if not (sel.p1 and sel.p2) then
		-- A single corner still gets a marker, so you can see what you picked.
		local single = sel.p1 or sel.p2
		if single then
			local obj = gs.spawn_beam(single, vector.new(1.05, 1.05, 1.05), "#ffe14080")
			if obj then
				sel.markers = {obj}
			end
		end
		return
	end

	local min, max = vector.sort(sel.p1, sel.p2)
	min = vector.subtract(min, 0.5)
	max = vector.add(max, 0.5)
	local span = vector.subtract(max, min)
	local centre = vector.add(min, vector.divide(span, 2))
	local t = 0.1

	for _, axis in ipairs({"x", "y", "z"}) do
		local a, b = unpack(({
			x = {"y", "z"}, y = {"x", "z"}, z = {"x", "y"},
		})[axis])
		for _, sa in ipairs({-1, 1}) do
			for _, sb in ipairs({-1, 1}) do
				local pos = vector.copy(centre)
				pos[a] = centre[a] + sa * span[a] / 2
				pos[b] = centre[b] + sb * span[b] / 2

				local size = vector.new(t, t, t)
				size[axis] = span[axis]

				local obj = gs.spawn_beam(pos, size, "#ffe140b0")
				if obj then
					sel.markers[#sel.markers + 1] = obj
				end
			end
		end
	end
end

local function get_selection(player_name)
	local sel = gs.selection[player_name]
	if not sel then
		sel = {markers = {}}
		gs.selection[player_name] = sel
	end
	return sel
end

function gs.set_corner(player_name, which, pos)
	local sel = get_selection(player_name)
	sel[which] = vector.round(pos)
	draw_markers(sel)
	return sel
end

function gs.get_region(player_name)
	local sel = gs.selection[player_name]
	if not (sel and sel.p1 and sel.p2) then
		return nil, "select two corners first (use the ghost wand, or /gs pos1 and /gs pos2)"
	end
	return vector.sort(sel.p1, sel.p2)
end

function gs.clear_selection(player_name)
	local sel = gs.selection[player_name]
	if sel then
		clear_markers(sel)
		gs.selection[player_name] = nil
	end
end

core.register_on_leaveplayer(function(player)
	gs.clear_selection(player:get_player_name())
end)

core.register_on_shutdown(function()
	for name in pairs(gs.selection) do
		gs.clear_selection(name)
	end
end)

--------------------------------------------------------------------------
-- Wand: pick the two corners of a region
--------------------------------------------------------------------------

core.register_tool("ghostschem:wand", {
	description = "Ghost Schematic Wand\n" ..
		"Punch: set corner 1\nPlace: set corner 2",
	inventory_image = "[fill:16x16:#3a2d1e^[fill:10x10:3,0:#ffe140",
	stack_max = 1,
	range = 12,

	on_use = function(_, user, pointed)
		if not (user and pointed and pointed.type == "node") then
			return
		end
		local name = user:get_player_name()
		gs.set_corner(name, "p1", pointed.under)
		core.chat_send_player(name,
			"Corner 1 set to " .. core.pos_to_string(pointed.under))
		return nil -- no wear
	end,

	on_place = function(itemstack, placer, pointed)
		if not (placer and pointed and pointed.type == "node") then
			return itemstack
		end
		local name = placer:get_player_name()
		gs.set_corner(name, "p2", pointed.under)
		core.chat_send_player(name,
			"Corner 2 set to " .. core.pos_to_string(pointed.under))
		return itemstack
	end,
})

--------------------------------------------------------------------------
-- Placer: drive the ghost preview
--------------------------------------------------------------------------

core.register_tool("ghostschem:placer", {
	description = "Ghost Schematic Placer\n" ..
		"Place: move the ghost here\n" ..
		"Sneak + place: rotate 90 degrees\n" ..
		"Punch: commit the ghost to the map",
	inventory_image = "[fill:16x16:#1e2a3a^[fill:10x10:3,3:#66ccff",
	stack_max = 1,
	range = 12,

	-- Punch commits. This is safe to bind to a single click precisely because
	-- the preview is already on screen: you are confirming something you can
	-- see, including everything it would overwrite.
	on_use = function(_, user)
		if not user then
			return
		end
		local name = user:get_player_name()
		local ok, result = gs.commit(name)
		if not ok then
			core.chat_send_player(name, "Ghost: " .. result)
			return
		end
		local msg = "Ghost placed."
		if result.overwrite > 0 then
			msg = msg .. " " .. result.overwrite .. " node(s) overwritten."
		end
		if result.skipped > 0 then
			msg = msg .. " " .. result.skipped .. " node(s) skipped."
		end
		msg = msg .. gs.describe_stack_result(result)
		core.chat_send_player(name, msg .. " Use /gs undo to revert.")
	end,

	on_place = function(itemstack, placer, pointed)
		if not placer then
			return itemstack
		end
		local name = placer:get_player_name()

		if placer:get_player_control().sneak then
			local preview = gs.get_preview(name)
			if not preview then
				core.chat_send_player(name, "Ghost: no active preview.")
				return itemstack
			end
			preview.rotation = gs.next_rotation(preview.rotation)
			preview:refresh()
			core.chat_send_player(name, "Ghost: rotation " .. preview.rotation)
			return itemstack
		end

		if not (pointed and pointed.type == "node") then
			return itemstack
		end

		local schem = gs.get_clipboard(name)
		if not schem then
			core.chat_send_player(name, "Ghost: clipboard is empty. Use /gs copy first.")
			return itemstack
		end

		local preview = gs.get_preview(name)
		local rotation = preview and preview.rotation or "0"
		local force = preview and preview.force
		if force == nil then
			force = true
		end

		preview = gs.show(name, schem, pointed.above, rotation, force)
		core.chat_send_player(name, "Ghost: " .. preview:summary())
		return itemstack
	end,
})
