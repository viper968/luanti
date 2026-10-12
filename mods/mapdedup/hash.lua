-- Block fingerprinting shared by the server and mapgen environments.
-- Both must produce identical strings for identical blocks.

local BS = 16
local tobit, band = bit.tobit, bit.band
local M1, M2 = 31, 65599
local C_IGNORE = core.get_content_id("ignore")

-- Content ids can change between restarts when mods change and the mapgen
-- environment has no mod storage, so node names are hashed instead: two
-- independent 24-bit ids per name.
local function name_ids(name)
	local a, b = 5381, 2166136
	for i = 1, #name do
		local c = name:byte(i)
		a = tobit(a * M1 + c)
		b = tobit(b * M2 + c)
	end
	return band(a, 0xFFFFFF), band(b, 0xFFFFFF)
end

local id1, id2 = {}, {}
local function fill(t, cid)
	id1[cid], id2[cid] = name_ids(core.get_name_from_content_id(cid))
	return t[cid]
end
setmetatable(id1, {__index = fill})
setmetatable(id2, {__index = fill})

-- Fingerprints every block in [bmin, bmax] (block coordinates) of a
-- VoxelManip's data. Calls cb(bp, hash) for each; hash is nil when the
-- block does not exist. extra(bp) may return a string to append, such as a
-- digest of the block's node metadata.
return function(data, p2data, area, bmin, bmax, cb, extra)
	local ystride, zstride = area.ystride, area.zstride
	for bz = bmin.z, bmax.z do
	for by = bmin.y, bmax.y do
	for bx = bmin.x, bmax.x do
		local h1, h2, ign = 0, 0, 0
		local zi = area:index(bx * BS, by * BS, bz * BS)
		for _ = 1, BS do
			local yi = zi
			for _ = 1, BS do
				for vi = yi, yi + BS - 1 do
					local cid, p2 = data[vi], p2data[vi]
					if cid == C_IGNORE then
						ign = ign + 1
					end
					h1 = tobit(h1 * M1 + id1[cid] * 256 + p2)
					h2 = tobit(h2 * M2 + id2[cid] * 256 + p2)
				end
				yi = yi + ystride
			end
			zi = zi + zstride
		end

		local bp = vector.new(bx, by, bz)
		local hash
		if ign < BS * BS * BS then
			hash = h1 .. ":" .. h2
			local e = extra and extra(bp)
			if e then
				hash = hash .. ":" .. e
			end
		end
		cb(bp, hash)
	end
	end
	end
end
