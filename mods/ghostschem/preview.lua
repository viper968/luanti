-- Preview lifecycle: one live ghost preview per player.

local gs = ghostschem

gs.previews = {}

local Preview = {}
Preview.__index = Preview

--------------------------------------------------------------------------
-- Conflict classification
--------------------------------------------------------------------------
-- This mirrors Schematic::blitToVManip() exactly, because a preview that
-- guesses differently from the engine is worse than no preview at all.
--
-- The engine's test for "may I write here" when force_placement is off is
-- literally:
--       content_t c = vm->m_data[vi].getContent();
--       if (c != CONTENT_AIR && c != CONTENT_IGNORE) continue;
--
-- Note it checks *only* air and ignore. `buildable_to` nodes like grass and
-- flowers are NOT treated as replaceable here, even though node placement
-- elsewhere in the game treats them that way. So we don't either.

local function classify(world_pos, node, force)
	local existing = core.get_node_or_nil(world_pos)
	if not existing or existing.name == "ignore" then
		return "unknown"
	end
	if existing.name == "air" then
		return "ok"
	end
	if force or node.force_place then
		return "overwrite"
	end
	return "skipped"
end

--------------------------------------------------------------------------
-- Preview object
--------------------------------------------------------------------------

function Preview.new(player_name, schem, origin, rotation, force)
	local self = setmetatable({}, Preview)
	self.player_name = player_name
	self.schem = schem
	self.origin = vector.round(origin)
	self.rotation = rotation or "0"
	self.force = force ~= false
	self.objects = {}
	self.counts = {shown = 0, total = 0, overwrite = 0, skipped = 0, unknown = 0}
	-- Bumped on every build so a late emerge callback can tell whether the
	-- preview it was started for is still the current one.
	self.generation = 0
	return self
end

function Preview:bounds()
	local size = gs.rotated_size(self.schem.size, self.rotation)
	local p1 = self.origin
	local p2 = vector.new(
		p1.x + size.x - 1,
		p1.y + size.y - 1,
		p1.z + size.z - 1
	)
	return p1, p2
end

function Preview:clear()
	for i = 1, #self.objects do
		local obj = self.objects[i]
		if obj and obj:get_pos() then
			obj:remove()
		end
	end
	self.objects = {}
end

-- Twelve beams along the edges of the bounding box. Used when a schematic is
-- too big to draw node-by-node.
function Preview:build_outline()
	local p1, p2 = self:bounds()
	-- Node centres sit on integers, so the box surface is half a node outside.
	local min = vector.subtract(p1, 0.5)
	local max = vector.add(p2, 0.5)
	local span = vector.subtract(max, min)
	local centre = vector.add(min, vector.divide(span, 2))
	local t = 0.12 -- beam thickness, in nodes

	local colour = self.counts.overwrite > 0 and "#ff3524d0" or "#66ccffd0"

	for _, axis in ipairs({"x", "y", "z"}) do
		local a, b = unpack(({
			x = {"y", "z"}, y = {"x", "z"}, z = {"x", "y"},
		})[axis])
		for _, sa in ipairs({-1, 1}) do
			for _, sb in ipairs({-1, 1}) do
				local pos = vector.new(centre)
				pos[a] = centre[a] + sa * span[a] / 2
				pos[b] = centre[b] + sb * span[b] / 2

				local size = vector.new()
				size[axis] = span[axis]
				size[a] = t
				size[b] = t

				local obj = gs.spawn_beam(pos, size, colour)
				if obj then
					self.objects[#self.objects + 1] = obj
				end
			end
		end
	end
end

-- core.load_area() deliberately does not generate map ("This function does not
-- trigger map generation"), so previewing into terrain that has never been
-- visited classifies every node as "unknown". That is honest but useless, so
-- kick off a real emerge and rebuild once it lands.
--
-- The emerge key is derived from the bounds, so moving or rotating the preview
-- naturally re-arms it, while a rebuild triggered by the callback itself sees
-- the same key and does not loop.
function Preview:request_emerge(p1, p2)
	local key = core.pos_to_string(p1) .. "/" .. core.pos_to_string(p2)
	if self.emerge_key == key then
		return
	end
	self.emerge_key = key

	local generation = self.generation
	core.emerge_area(p1, p2, function(_, _, calls_remaining)
		if calls_remaining ~= 0 then
			return
		end
		-- Bail if the preview was hidden, replaced, or moved while emerging.
		if gs.previews[self.player_name] ~= self or self.generation ~= generation then
			return
		end
		self:build()
	end)
end

function Preview:build()
	self:clear()
	self.generation = self.generation + 1

	local rotated = gs.rotate(self.schem, self.rotation)
	self.rotated = rotated

	local counts = {shown = 0, total = 0, overwrite = 0, skipped = 0, unknown = 0}
	self.counts = counts

	-- Make sure the target area is loaded before classifying, otherwise every
	-- ghost comes back "unknown" and the preview tells you nothing.
	local p1, p2 = self:bounds()
	core.load_area(p1, p2)

	local shown, total = gs.count_visible(rotated)
	counts.shown, counts.total = shown, total

	if shown > gs.settings.max_entities then
		self.outlined = true
		-- Still classify, so the outline can be coloured honestly, but do it
		-- without spawning anything.
		local origin, world = self.origin, vector.new()
		gs.each_visible(rotated, function(x, y, z, node)
			world.x, world.y, world.z = origin.x + x, origin.y + y, origin.z + z
			local variant = classify(world, node, self.force)
			if variant ~= "ok" then
				counts[variant] = counts[variant] + 1
			end
		end)
		self:build_outline()
		if counts.unknown > 0 then
			self:request_emerge(p1, p2)
		end
		return
	end

	self.outlined = false
	local origin = self.origin
	gs.each_visible(rotated, function(x, y, z, node)
		local world = vector.new(origin.x + x, origin.y + y, origin.z + z)
		local variant = classify(world, node, self.force)
		if variant ~= "ok" then
			counts[variant] = counts[variant] + 1
		end
		local obj = gs.spawn_ghost(world, node.name, variant)
		if obj then
			self.objects[#self.objects + 1] = obj
		end
	end)

	if counts.unknown > 0 then
		self:request_emerge(p1, p2)
	end
end

function Preview:refresh()
	self:build()
end

function Preview:summary()
	local c = self.counts
	local size = gs.rotated_size(self.schem.size, self.rotation)
	local parts = {
		string.format("%dx%dx%d at %s, rotation %s",
			size.x, size.y, size.z,
			core.pos_to_string(self.origin), self.rotation),
		string.format("%d nodes (%d ghosts drawn%s)",
			c.total, c.shown, self.outlined and ", outline only" or ""),
	}
	if c.overwrite > 0 then
		parts[#parts + 1] = string.format("%d would be OVERWRITTEN", c.overwrite)
	end
	if c.skipped > 0 then
		parts[#parts + 1] = string.format("%d would be SKIPPED", c.skipped)
	end
	if c.unknown > 0 then
		parts[#parts + 1] = string.format("%d in unloaded area", c.unknown)
	end
	parts[#parts + 1] = self.force and "force_placement: on" or "force_placement: off"
	return table.concat(parts, "; ")
end

--------------------------------------------------------------------------
-- Registry
--------------------------------------------------------------------------

function gs.get_preview(player_name)
	return gs.previews[player_name]
end

function gs.show(player_name, schem, origin, rotation, force)
	gs.hide(player_name)
	local preview = Preview.new(player_name, schem, origin, rotation, force)
	gs.previews[player_name] = preview
	preview:build()
	return preview
end

function gs.hide(player_name)
	local preview = gs.previews[player_name]
	if preview then
		preview:clear()
		gs.previews[player_name] = nil
	end
	return preview ~= nil
end

--------------------------------------------------------------------------
-- Keeping previews honest
--------------------------------------------------------------------------
-- If the world changes underneath a preview, the conflict colouring is stale.
-- Rebuilding on every node change would be far too expensive, so we only
-- schedule a rebuild when the change lands inside a preview's bounding box,
-- and we coalesce bursts (a WorldEdit paste, a falling node cascade) into one
-- rebuild on the next server step.

local dirty = {}
local has_dirty = false

local function mark_dirty_at(pos)
	for name, preview in pairs(gs.previews) do
		local p1, p2 = preview:bounds()
		if pos.x >= p1.x and pos.x <= p2.x
				and pos.y >= p1.y and pos.y <= p2.y
				and pos.z >= p1.z and pos.z <= p2.z then
			dirty[name] = true
			has_dirty = true
		end
	end
end

core.register_on_placenode(function(pos)
	mark_dirty_at(pos)
end)

core.register_on_dignode(function(pos)
	mark_dirty_at(pos)
end)

core.register_globalstep(function()
	if not has_dirty then
		return
	end
	has_dirty = false
	for name in pairs(dirty) do
		dirty[name] = nil
		local preview = gs.previews[name]
		if preview then
			preview:refresh()
		end
	end
end)

core.register_on_leaveplayer(function(player)
	gs.hide(player:get_player_name())
end)

core.register_on_shutdown(function()
	for name in pairs(gs.previews) do
		gs.hide(name)
	end
end)
