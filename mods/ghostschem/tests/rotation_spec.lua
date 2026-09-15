-- Verify ghostschem's rotation mapping against a direct transcription of
-- Schematic::blitToVManip() from src/mapgen/mg_schematic.cpp.

core = {registered_nodes = {}}
vector = {}
ghostschem = {}
dofile("/home/user/luanti/mods/ghostschem/api.lua")
local gs = ghostschem

-- Direct port of the engine's index arithmetic.
-- Returns dest[x][y][z] -> source array index (0-based), and the dest size.
local function blit(size, rot)
	local xstride = 1
	local ystride = size.x
	local zstride = size.x * size.y

	local sx, sy, sz = size.x, size.y, size.z
	local i_start, i_step_x, i_step_z

	if rot == "90" then
		i_start  = sx - 1
		i_step_x = zstride
		i_step_z = -xstride
		sx, sz = sz, sx
	elseif rot == "180" then
		i_start  = zstride * (sz - 1) + sx - 1
		i_step_x = -xstride
		i_step_z = -zstride
	elseif rot == "270" then
		i_start  = zstride * (sz - 1)
		i_step_x = -zstride
		i_step_z = xstride
		sx, sz = sz, sx
	else
		i_start  = 0
		i_step_x = xstride
		i_step_z = zstride
	end

	local map = {}
	for y = 0, sy - 1 do
		for z = 0, sz - 1 do
			local i = z * i_step_z + y * ystride + i_start
			for x = 0, sx - 1 do
				map[string.format("%d,%d,%d", x, y, z)] = i
				i = i + i_step_x
			end
		end
	end
	return map, {x = sx, y = sy, z = sz}
end

local failures = 0
local checks = 0

-- Deliberately asymmetric sizes, so a transposed axis cannot pass by accident.
for _, size in ipairs({
	{x = 3, y = 2, z = 5},
	{x = 1, y = 1, z = 4},
	{x = 7, y = 3, z = 2},
	{x = 2, y = 2, z = 2},
}) do
	-- data[i] carries its own 0-based source index as a tag.
	local schem = {size = size, data = {}}
	local n = size.x * size.y * size.z
	for i = 0, n - 1 do
		schem.data[i + 1] = {name = "tag", src = i}
	end

	for _, rot in ipairs({"0", "90", "180", "270"}) do
		local expect_map, expect_size = blit(size, rot)
		local got = gs.rotate(schem, rot)
		local got_size = gs.rotated_size(size, rot)

		assert(got_size.x == expect_size.x and got_size.y == expect_size.y
			and got_size.z == expect_size.z,
			string.format("size mismatch for rot %s on %dx%dx%d: got %dx%dx%d expected %dx%dx%d",
				rot, size.x, size.y, size.z,
				got_size.x, got_size.y, got_size.z,
				expect_size.x, expect_size.y, expect_size.z))

		for z = 0, got_size.z - 1 do
			for y = 0, got_size.y - 1 do
				for x = 0, got_size.x - 1 do
					checks = checks + 1
					local key = string.format("%d,%d,%d", x, y, z)
					local expected_src = expect_map[key]
					local node = gs.get(got, x, y, z)
					if not node then
						failures = failures + 1
						print(string.format("MISSING rot=%s size=%dx%dx%d at %s",
							rot, size.x, size.y, size.z, key))
					elseif node.src ~= expected_src then
						failures = failures + 1
						print(string.format("MISMATCH rot=%s size=%dx%dx%d at %s: got src=%d expected src=%d",
							rot, size.x, size.y, size.z, key, node.src, expected_src))
					end
				end
			end
		end

		-- The rotation must be a bijection: no holes, no doubled writes.
		local count = 0
		for _ in pairs(got.data) do count = count + 1 end
		assert(count == n, string.format(
			"rot %s on %dx%dx%d produced %d entries, expected %d (holes break place_schematic's ipairs read)",
			rot, size.x, size.y, size.z, count, n))
	end
end

print(string.format("%d positions checked, %d failures", checks, failures))
os.exit(failures == 0 and 0 or 1)
