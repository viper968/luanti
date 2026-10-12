-- Runs in the mapgen environment, off the main server thread: fingerprints
-- what the generator wrote and passes it to the server via gennotify.

local hash_blocks = dofile(core.get_modpath("mapdedup") .. "/hash.lua")

local data, p2data = {}, {} -- reused to avoid allocating huge tables

core.register_on_mods_loaded(function()
	-- Registered here so it runs after other mods' mapgen callbacks.
	core.register_on_generated(function(vm, minp, maxp)
		local emin, emax = vm:get_emerged_area()
		vm:get_data(data)
		vm:get_param2_data(p2data)
		local hashes = {}
		-- The emerged area is the chunk plus the one-block border shell the
		-- generator may also write to.
		hash_blocks(data, p2data, VoxelArea(emin, emax),
			vector.floor(vector.divide(emin, 16)),
			vector.floor(vector.divide(emax, 16)),
			function(bp, hash)
				hashes[core.hash_node_position(bp)] = hash or false
			end)
		core.save_gen_notify("mapdedup", hashes)
	end)
end)
