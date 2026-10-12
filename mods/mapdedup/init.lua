-- mapdedup: delete mapchunks that nothing but the map generator has touched.
--
-- How it works:
--  * Every time the engine generates a mapchunk, every mapblock the generator
--    wrote to (the chunk plus its one-block border shell) is fingerprinted:
--    node names and param2, plus node metadata where available. Light
--    (param1) is ignored because it changes on its own. By default this
--    happens in the mapgen environment (mapgen.lua, off the main thread);
--    see the mapdedup.record_in setting. The server stores these per chunk.
--  * A check/clean job re-fingerprints a chunk and its border shell. If every
--    block still matches what the generator last
--    wrote, nothing else has changed it, so the whole chunk is deleted and
--    will be regenerated the next time someone goes there. Whole chunks are
--    deleted because regenerating any block regenerates its whole chunk and
--    writes into the border shell around it.
--
-- The comparison is against the stored fingerprint, never against a fresh
-- regeneration, so mapgen non-determinism can never cause a modified block
-- to be mistaken for an untouched one.

local modpath = core.get_modpath("mapdedup")
local storage = core.get_mod_storage()
local hash_blocks = dofile(modpath .. "/hash.lua")

local BS = core.MAP_BLOCKSIZE

local step_budget_us = (tonumber(core.settings:get("mapdedup.step_budget")) or 30) * 1000
local player_margin = tonumber(core.settings:get("mapdedup.player_margin")) or 64
local debug_timing = core.settings:get_bool("mapdedup.debug", false)
local record_on_server = core.settings:get("mapdedup.record_in") == "server"

if not record_on_server then
	core.register_mapgen_script(modpath .. "/mapgen.lua")
	core.set_gen_notify({custom = true}, nil, {"mapdedup"})
end

local csize -- chunk size in blocks (vector), known once mods are loaded
local cur_fp -- index of the current mapgen fingerprint

--
-- Mapgen fingerprint: chunks generated under different mapgen settings or
-- mapgen-related registrations would regenerate differently from their
-- neighbours, so they are never deleted.
--

local function sorted_keys(t)
	local keys = {}
	for k in pairs(t) do
		keys[#keys + 1] = k
	end
	table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
	return keys
end

local function describe(v)
	if type(v) == "table" then
		local parts = {}
		for _, k in ipairs(sorted_keys(v)) do
			parts[#parts + 1] = tostring(k) .. "=" .. describe(v[k])
		end
		return "{" .. table.concat(parts, ",") .. "}"
	elseif type(v) == "function" or type(v) == "userdata" then
		return type(v)
	end
	return tostring(v)
end

local function compute_fingerprint()
	local parts = {}
	for _, s in ipairs({"seed", "mg_name", "chunksize", "water_level",
			"mg_flags", "mapgen_limit"}) do
		parts[#parts + 1] = s .. "=" .. tostring(core.get_mapgen_setting(s))
	end
	local mg = core.get_mapgen_setting("mg_name")
	-- mapgen-specific settings such as mgv7_spflags or mgv7_np_terrain_base
	for _, s in ipairs({"spflags", "np_terrain_base", "np_terrain_alt",
			"np_terrain", "np_mountain", "np_ridge", "np_cave1", "np_cave2",
			"cave_width", "large_cave_depth", "cavern_limit"}) do
		local v = core.get_mapgen_setting(mg .. "_" .. s)
		if v then
			parts[#parts + 1] = s .. "=" .. v
		end
	end
	for _, list in ipairs({"registered_biomes", "registered_decorations",
			"registered_ores"}) do
		local entries = {}
		for _, def in pairs(core[list]) do
			entries[#entries + 1] = describe(def)
		end
		table.sort(entries)
		parts[#parts + 1] = list .. ":" .. table.concat(entries, ";")
	end
	-- Other mods' on_generated callbacks also shape the terrain.
	parts[#parts + 1] = "on_generated=" .. #core.registered_on_generateds
	return core.sha1(table.concat(parts, "\n"))
end

local function fingerprint_index(fp)
	local i = 0
	for line in storage:get_string("fingerprints"):gmatch("[^\n]+") do
		i = i + 1
		if line == fp then
			return i
		end
	end
	local list = storage:get_string("fingerprints")
	storage:set_string("fingerprints", list .. fp .. "\n")
	return i + 1
end

--
-- Chunk geometry (positions in block coordinates unless noted)
--

local function chunk_origin_axis(b, cs)
	local off = -math.floor(cs / 2)
	return math.floor((b - off) / cs) * cs + off
end

local function chunk_origin(bp)
	return vector.new(chunk_origin_axis(bp.x, csize.x),
		chunk_origin_axis(bp.y, csize.y), chunk_origin_axis(bp.z, csize.z))
end

local function chunk_key(o)
	return "c" .. o.x .. "," .. o.y .. "," .. o.z
end

local function block_index(bp, o)
	return ((bp.z - o.z) * csize.y + (bp.y - o.y)) * csize.x + (bp.x - o.x)
end

--
-- Per-chunk records. Stored as "g<fp>" or "-" followed by ";<index>=<hash>"
-- for every block of the chunk the generator has written to.
--

local function load_rec(key)
	local s = storage:get_string(key)
	local rec = {fp = nil, h = {}}
	if s == "" then
		return rec
	end
	rec.fp = tonumber(s:match("^g(%d+)"))
	for i, h in s:gmatch(";(%d+)=([^;]+)") do
		rec.h[tonumber(i)] = h
	end
	return rec
end

local function save_rec(key, rec)
	local parts = {rec.fp and ("g" .. rec.fp) or "-"}
	local empty = true
	for i, h in pairs(rec.h) do
		parts[#parts + 1] = ";" .. i .. "=" .. h
		empty = false
	end
	if empty and not rec.fp then
		storage:set_string(key, "")
	else
		storage:set_string(key, table.concat(parts))
	end
end

--
-- Hashing
--

local function canon_meta(pos)
	local t = core.get_meta(pos):to_table()
	local parts = {}
	local fields = t.fields or {}
	for _, k in ipairs(sorted_keys(fields)) do
		local v = tostring(fields[k])
		parts[#parts + 1] = #k .. ":" .. k .. #v .. ":" .. v
	end
	local inv = t.inventory or {}
	for _, ln in ipairs(sorted_keys(inv)) do
		parts[#parts + 1] = "[" .. #ln .. ":" .. ln
		for _, st in ipairs(inv[ln]) do
			local s = type(st) == "string" and st or st:to_string()
			parts[#parts + 1] = #s .. ":" .. s
		end
	end
	return table.concat(parts)
end

-- Fingerprints the blocks in [bmin, bmax] as they are now, including node
-- metadata. Calls cb(bp, hash) for each; hash is nil when the block does
-- not exist.
local data, p2data = {}, {} -- reused to avoid allocating huge tables
local function hash_region(bmin, bmax, cb)
	local pmin = vector.multiply(bmin, BS)
	local pmax = vector.add(vector.multiply(bmax, BS), BS - 1)

	local vm = VoxelManip()
	local emin, emax = vm:read_from_map(pmin, pmax)
	vm:get_data(data)
	vm:get_param2_data(p2data)

	local metas = {}
	for _, pos in ipairs(core.find_nodes_with_meta(pmin, pmax)) do
		local bk = core.hash_node_position(vector.floor(vector.divide(pos, BS)))
		metas[bk] = metas[bk] or {}
		table.insert(metas[bk], pos)
	end

	hash_blocks(data, p2data, VoxelArea(emin, emax), bmin, bmax, cb, function(bp)
		local list = metas[core.hash_node_position(bp)]
		if not list then
			return nil
		end
		table.sort(list, function(p, q)
			return core.hash_node_position(p) < core.hash_node_position(q)
		end)
		local parts = {}
		for _, pos in ipairs(list) do
			parts[#parts + 1] = core.pos_to_string(pos) .. canon_meta(pos)
		end
		return core.sha1(table.concat(parts, "\n"))
	end)
end

--
-- Recording what the generator wrote
--

-- Fingerprints of the chunk at minp..maxp and its border shell, keyed by
-- block position hash; false for blocks that do not exist.
local function fingerprints_of_new_chunk(minp, maxp)
	if not record_on_server then
		local gennotify = core.get_mapgen_object("gennotify")
		return gennotify.custom and gennotify.custom["mapdedup"]
	end
	local hashes = {}
	hash_region(vector.subtract(vector.floor(vector.divide(minp, BS)), 1),
		vector.add(vector.floor(vector.divide(maxp, BS)), 1),
		function(bp, hash)
			hashes[core.hash_node_position(bp)] = hash or false
		end)
	return hashes
end

local function on_generated(minp, maxp)
	local t0 = core.get_us_time()
	local hashes = fingerprints_of_new_chunk(minp, maxp)
	if not hashes then
		core.log("warning", "[mapdedup] no fingerprints for chunk at " ..
			core.pos_to_string(minp) .. "; it will never be deleted")
		return
	end

	local recs = {}
	for bk, hash in pairs(hashes) do
		local bp = core.get_position_from_hash(bk)
		local o = chunk_origin(bp)
		local key = chunk_key(o)
		local rec = recs[key]
		if not rec then
			rec = load_rec(key)
			recs[key] = rec
		end
		rec.h[block_index(bp, o)] = hash or nil
	end
	local this_key = chunk_key(chunk_origin(vector.floor(vector.divide(minp, BS))))
	recs[this_key].fp = cur_fp
	for key, rec in pairs(recs) do
		save_rec(key, rec)
	end

	if debug_timing then
		core.log("action", ("[mapdedup] recorded chunk %s in %.1f ms"):format(
			core.pos_to_string(minp), (core.get_us_time() - t0) / 1000))
	end
end

core.register_on_mods_loaded(function()
	csize = core.get_mapgen_chunksize()
	local old = storage:get_string("chunksize")
	local now = core.pos_to_string(csize)
	if old ~= "" and old ~= now then
		core.log("warning", "[mapdedup] chunksize changed from " .. old ..
			" to " .. now .. "; forgetting all recorded chunks")
		for _, key in ipairs(storage:get_keys()) do
			if key:sub(1, 1) == "c" then
				storage:set_string(key, "")
			end
		end
	end
	storage:set_string("chunksize", now)
	cur_fp = fingerprint_index(compute_fingerprint())
	-- Registered here so that in "server" mode it runs after every other
	-- mod's on_generated callback and sees their changes as generated terrain.
	core.register_on_generated(on_generated)
end)

--
-- Checking and deleting
--

local function player_near(pmin, pmax)
	for _, player in ipairs(core.get_connected_players()) do
		local p = player:get_pos()
		if p.x >= pmin.x - player_margin and p.x <= pmax.x + player_margin and
				p.y >= pmin.y - player_margin and p.y <= pmax.y + player_margin and
				p.z >= pmin.z - player_margin and p.z <= pmax.z + player_margin then
			return true
		end
	end
	return false
end

-- Returns a status string: "deleted", "deletable" or the reason it was kept.
local function process_chunk(o, dry_run)
	local key = chunk_key(o)
	local rec = load_rec(key)
	if not rec.fp then
		return "untracked"
	end
	if rec.fp ~= cur_fp then
		return "mapgen_changed"
	end

	local omax = vector.subtract(vector.add(o, csize), 1)
	local pmin = vector.multiply(o, BS)
	local pmax = vector.add(vector.multiply(omax, BS), BS - 1)
	if player_near(pmin, pmax) then
		return "player_near"
	end

	local recs = {[key] = rec}
	local changed = false
	hash_region(vector.subtract(o, 1), vector.add(omax, 1), function(bp, hash)
		if changed then
			return
		end
		local bo = chunk_origin(bp)
		local bkey = chunk_key(bo)
		local brec = recs[bkey]
		if not brec then
			brec = load_rec(bkey)
			recs[bkey] = brec
		end
		if brec.h[block_index(bp, bo)] ~= hash then
			changed = true
		end
	end)
	if changed then
		return "modified"
	end

	for obj in core.objects_in_area(pmin, pmax) do
		if obj then
			return "has_objects"
		end
	end

	if dry_run then
		return "deletable"
	end
	core.delete_area(pmin, pmax)
	storage:set_string(key, "")
	return "deleted"
end

local job

local function report(text)
	if job and job.name and core.get_player_by_name(job.name) then
		core.chat_send_player(job.name, "[mapdedup] " .. text)
	end
	core.log("action", "[mapdedup] " .. text)
end

local function format_stats(stats)
	local parts = {}
	for _, k in ipairs(sorted_keys(stats)) do
		parts[#parts + 1] = k .. "=" .. stats[k]
	end
	return table.concat(parts, ", ")
end

core.register_globalstep(function()
	if not job then
		return
	end
	local t0 = core.get_us_time()
	repeat
		local o = job.queue[job.i]
		if not o then
			report((job.dry_run and "Check" or "Clean") .. " finished: " ..
				format_stats(job.stats))
			job = nil
			return
		end
		local status = process_chunk(o, job.dry_run)
		job.stats[status] = (job.stats[status] or 0) + 1
		job.i = job.i + 1
	until core.get_us_time() - t0 >= step_budget_us

	if t0 - job.last_report > 10 * 1000000 then
		job.last_report = t0
		report(("%d/%d chunks: %s"):format(job.i - 1, #job.queue,
			format_stats(job.stats)))
	end
end)

local function queue_all()
	local queue = {}
	for _, key in ipairs(storage:get_keys()) do
		local x, y, z = key:match("^c(%-?%d+),(%-?%d+),(%-?%d+)$")
		-- Records holding only border blocks belong to chunks that were
		-- never generated themselves.
		if x and storage:get_string(key):sub(1, 1) == "g" then
			queue[#queue + 1] = vector.new(tonumber(x), tonumber(y), tonumber(z))
		end
	end
	return queue
end

local function queue_radius(center, r)
	local o = chunk_origin(vector.floor(vector.divide(center, BS)))
	local queue = {}
	for dz = -r, r do
	for dy = -r, r do
	for dx = -r, r do
		queue[#queue + 1] = vector.add(o, vector.multiply(csize, vector.new(dx, dy, dz)))
	end
	end
	end
	return queue
end

core.register_chatcommand("mapdedup", {
	params = "check [<radius>|all] | clean [<radius>|all] | stop | status",
	description = "Find (check) or delete (clean) mapchunks that only the " ..
		"map generator has changed. <radius> is in mapchunks around you.",
	privs = {server = true},
	func = function(name, param)
		local cmd, arg = param:match("^(%S*)%s*(%S*)$")
		if cmd == "stop" then
			if not job then
				return false, "No job is running."
			end
			local stats = format_stats(job.stats)
			job = nil
			return true, "Stopped. So far: " .. stats
		elseif cmd == "status" then
			if not job then
				return true, "No job is running."
			end
			return true, ("%d/%d chunks: %s"):format(job.i - 1, #job.queue,
				format_stats(job.stats))
		elseif cmd ~= "check" and cmd ~= "clean" then
			return false, "Usage: /mapdedup " .. "check [<radius>|all] | " ..
				"clean [<radius>|all] | stop | status"
		end
		if job then
			return false, "A job is already running; use /mapdedup stop first."
		end

		local queue
		if arg == "all" then
			queue = queue_all()
		else
			local r = tonumber(arg ~= "" and arg or "2")
			local player = core.get_player_by_name(name)
			if not r or r < 0 or r ~= math.floor(r) then
				return false, "Radius must be a whole number of mapchunks or 'all'."
			elseif not player then
				return false, "A radius needs a player position; use 'all' from the console."
			end
			queue = queue_radius(player:get_pos(), r)
		end
		job = {
			queue = queue,
			i = 1,
			dry_run = cmd == "check",
			stats = {},
			name = name,
			last_report = core.get_us_time(),
		}
		return true, ("Started %s of %d mapchunks."):format(cmd, #queue)
	end,
})
