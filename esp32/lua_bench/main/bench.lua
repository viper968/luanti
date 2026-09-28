-- Lua 5.1 workloads modelled on what Luanti's builtin and mods do.
-- Each returns a checksum so the double and float builds can be compared
-- for correctness as well as speed. Loop counts and intermediate integers
-- stay below 2^24 unless a test is *meant* to show precision loss.
local floor, sqrt = math.floor, math.sqrt
local B, order = {}, {}
local function bench(name, fn) B[name] = fn; order[#order + 1] = name end

-- Plain integer arithmetic: loop counters, indices, item counts
bench("int_loop", function()
	local s = 0
	for i = 1, 400000 do s = s + (i % 7) * 3 - 1 end
	return s
end)

-- Fractional maths on locals: entity physics step
bench("physics_step", function()
	local px, py, pz, vx, vy, vz = 0.5, 10.25, -3.75, 0.1, 0.0, -0.2
	local acc = 0
	for i = 1, 100000 do
		vy = vy - 9.81 * 0.05
		px, py, pz = px + vx * 0.05, py + vy * 0.05, pz + vz * 0.05
		if py < 0 then py = -py; vy = -vy * 0.5 end
		acc = acc + sqrt(vx * vx + vy * vy + vz * vz)
	end
	return floor(acc)
end)

-- vector.* style: small tables with a metatable, lots of allocation
bench("vector_tables", function()
	local mt = {}
	mt.__index = mt
	local function new(x, y, z) return setmetatable({x = x, y = y, z = z}, mt) end
	mt.__add = function(a, b) return new(a.x + b.x, a.y + b.y, a.z + b.z) end
	function mt.scale(a, s) return new(a.x * s, a.y * s, a.z * s) end
	function mt.length(a) return sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
	local p, v, acc = new(0, 0, 0), new(0.1, 0.2, -0.3), 0
	for i = 1, 30000 do
		p = p + v:scale(0.5)
		acc = acc + p:length() * 0.001
	end
	return floor(acc)
end)

-- core.hash_node_position over a 32^3 area, used as table keys.
-- Exact numbers give 32768 unique keys; fewer means positions collided.
bench("hash_positions", function()
	local function hash(x, y, z)
		return (z + 0x8000) * 0x100000000 + (y + 0x8000) * 0x10000 + (x + 0x8000)
	end
	local seen, unique = {}, 0
	for z = -16, 15 do for y = -16, 15 do for x = -16, 15 do
		local h = hash(x, y, z)
		if not seen[h] then seen[h] = true; unique = unique + 1 end
	end end end
	return unique
end)

-- Inventory-like tables: build, iterate with ipairs/pairs, string keys
bench("table_churn", function()
	local t = {}
	for i = 1, 20000 do
		t[#t + 1] = {name = "default:stone", count = i % 99 + 1, wear = 0}
	end
	local s = 0
	for _, st in ipairs(t) do s = s + st.count end
	local m = {}
	for i = 1, 5000 do m["key" .. (i % 500)] = i end
	for _, v in pairs(m) do s = s + v end
	while #t > 10000 do t[#t] = nil end
	return s + #t
end)

-- Formspec building and parsing: string.format, concat, gmatch, tonumber
bench("strings", function()
	local parts = {}
	for i = 1, 3000 do
		parts[#parts + 1] = string.format("item_image_button[%d,%d;1,1;%s;btn_%d;%d]",
			i % 8, floor(i / 8) % 4, "default:dirt", i, i % 99)
	end
	local s = table.concat(parts)
	local n = 0
	for num in s:gmatch("btn_(%d+)") do n = n + tonumber(num) end
	return n + #s
end)

-- ABM-like scan of a 16^3 map block with a cheap PRNG chance per node
bench("abm_scan", function()
	local ids = {}
	for i = 0, 4095 do ids[i] = (i * 7919) % 5 end
	local changed, seed = 0, 12345
	for pass = 1, 5 do
		for z = 0, 15 do for y = 0, 15 do for x = 0, 15 do
			local idx = z * 256 + y * 16 + x
			if ids[idx] == 2 then
				seed = (seed * 75 + 74) % 65537
				if seed % 50 == 0 then ids[idx] = 3; changed = changed + 1 end
			end
		end end end
	end
	return changed
end)

-- Registered callbacks called in a loop (on_step, globalsteps, ...)
bench("callbacks", function()
	local cbs = {}
	for i = 1, 50 do local k = i; cbs[i] = function(a, b) return a + b * k end end
	local s = 0
	for r = 1, 2000 do
		for i = 1, #cbs do s = (s + cbs[i](r, 1)) % 1000003 end
	end
	return s
end)

local total = 0
for _, name in ipairs(order) do
	local best, res = nil, nil
	for rep = 1, 3 do
		collectgarbage("collect")
		local dt, r = timeit(B[name])  -- microseconds, measured in C
		if not best or dt < best then best = dt end
		res = r
	end
	total = total + best
	print(string.format("BENCH %-15s %9.1f ms  result=%s", name, best / 1000, tostring(res)))
end
print(string.format("BENCH %-15s %9.1f ms", "TOTAL", total / 1000))

-- Memory: 20000 position tables {x=, y=, z=}, like a mod caching nodes
collectgarbage("collect")
local before, sys_before = collectgarbage("count"), sysfree_kb()
local keep = {}
for i = 1, 20000 do keep[i] = {x = i * 0.5, y = i, z = -i} end
collectgarbage("collect")
-- Lua's own count vs what the heap actually lost (includes allocator overhead)
print(string.format("MEM 20000 position tables: %.0f KB (Lua count), %.0f KB (real heap)",
	collectgarbage("count") - before, sys_before - sysfree_kb()))
keep = nil
collectgarbage("collect")
print(string.format("MEM Lua heap after benchmarks: %.0f KB", collectgarbage("count")))
