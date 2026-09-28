-- Moves each connected player at walking speed in its own direction (player 1
-- east, player 2 north, ...), a few nodes above the terrain. This keeps
-- loading and generating new map blocks, the worst case for server memory.
-- Every few seconds it places a node under the player so there are map
-- changes to save.
local SPEED = 4.0 -- nodes per second, roughly walking speed
local dirs = {{x = 1, z = 0}, {x = 0, z = 1}, {x = -1, z = 0}, {x = 0, z = -1}}
local state = {}
local placed = 0

local function surface_y(x, z, fallback)
	for y = 40, -20, -1 do
		local name = core.get_node({x = x, y = y, z = z}).name
		if name ~= "air" and name ~= "ignore" then
			return y + 1
		end
	end
	return fallback
end

core.register_on_joinplayer(function(player)
	local n = #core.get_connected_players()
	state[player:get_player_name()] = {dir = dirs[(n - 1) % #dirs + 1], place_timer = 0}
	player:set_pos({x = 0, y = 20, z = 0})
end)

core.register_on_leaveplayer(function(player)
	state[player:get_player_name()] = nil
end)

core.register_globalstep(function(dtime)
	for _, player in ipairs(core.get_connected_players()) do
		local s = state[player:get_player_name()]
		if s then
			local pos = player:get_pos()
			local nx, nz = pos.x + s.dir.x * SPEED * dtime, pos.z + s.dir.z * SPEED * dtime
			local ny = surface_y(math.floor(nx + 0.5), math.floor(nz + 0.5), pos.y) + 2
			player:set_pos({x = nx, y = ny, z = nz})
			s.place_timer = s.place_timer + dtime
			if s.place_timer > 3 then
				s.place_timer = 0
				core.set_node(vector.round({x = nx, y = ny - 1, z = nz}), {name = "tiny:wood"})
				placed = placed + 1
			end
		end
	end
end)

-- One status line per 10 s so the test log shows progress
local t = 0
core.register_globalstep(function(dtime)
	t = t + dtime
	if t < 10 then return end
	t = 0
	local parts = {}
	for _, p in ipairs(core.get_connected_players()) do
		local pos = vector.round(p:get_pos())
		parts[#parts + 1] = p:get_player_name() .. "@" .. core.pos_to_string(pos)
	end
	core.log("action", "[walker] players: " .. table.concat(parts, " ") .. " | nodes placed: " .. placed)
end)
