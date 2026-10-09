-- Emerges a fixed area around the origin, then shuts the server down.
-- Used to generate test worlds for the mapblock codec benchmark.
core.after(1, function()
	local t0 = core.get_us_time()
	core.emerge_area({x=-320, y=-96, z=-320}, {x=319, y=95, z=319}, function(bp, action, remaining)
		if remaining == 0 then
			core.log("action", "EMERGE DONE in " .. (core.get_us_time() - t0) / 1e6 .. "s")
			core.request_shutdown("done")
		end
	end)
end)
