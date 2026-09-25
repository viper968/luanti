-- In-engine test suite.
--
-- The whole point of a ghost preview is that it tells the truth, so this
-- checks it against the live engine rather than against assumptions:
--
--   1. gs.rotate() predicts exactly what core.place_schematic() writes,
--      for all four rotations.
--   2. Conflicts are classified the same way blitToVManip() decides them,
--      in both force_placement modes.
--   3. Committing writes the schematic, and undo restores the region exactly.
--   4. Oversized schematics fall back to a 12-beam outline.
--   5. A preview over ungenerated map emerges it and reclassifies itself.
--
-- The whole test volume is snapshotted with a VoxelManip and restored
-- afterwards, so running it does not damage the world.
--
-- Run it in-game with /gs selftest, or headlessly on server start with
--   ghostschem_selftest = true
--   ghostschem_selftest_on_start = true

local gs = ghostschem

local PS = {x = 3, y = 2, z = 5}   -- asymmetric on purpose: a symmetric test
                                   -- shape would hide transposed-axis bugs
local TEST_PLAYER = "__ghostschem_selftest__"

local function palette_for()
	-- Pick four distinguishable registered nodes, whatever game we are on.
	local found = {}
	for name, def in pairs(core.registered_nodes) do
		if (def.drawtype == nil or def.drawtype == "normal")
				and name ~= "air" and name ~= "ignore"
				and not (def.groups or {}).not_in_creative_inventory then
			found[#found + 1] = name
			if #found >= 4 then
				break
			end
		end
	end
	return found
end

local function pattern_node(palette, x, y, z)
	if (x + y + z) % 3 == 0 then
		return "air"
	end
	return palette[((x * 7 + y * 13 + z * 3) % #palette) + 1]
end

local function snapshot(p1, p2)
	local vm = core.get_voxel_manip()
	local emin, emax = vm:read_from_map(p1, p2)
	return {p1 = p1, p2 = p2, emin = emin, emax = emax,
		data = vm:get_data(), param2 = vm:get_param2_data()}
end

local function restore(snap)
	local vm = core.get_voxel_manip()
	vm:read_from_map(snap.p1, snap.p2)
	vm:set_data(snap.data)
	vm:set_param2_data(snap.param2)
	vm:write_to_map(true)
end

local function read_region(p1, size)
	local out = {}
	for z = 0, size.z - 1 do
		for y = 0, size.y - 1 do
			for x = 0, size.x - 1 do
				out[#out + 1] = core.get_node(
					vector.add(p1, vector.new(x, y, z))).name
			end
		end
	end
	return out
end

local function first_difference(a, b)
	if #a ~= #b then
		return 0
	end
	for i = 1, #a do
		if a[i] ~= b[i] then
			return i
		end
	end
	return nil
end

--------------------------------------------------------------------------
-- The suite
--------------------------------------------------------------------------

local function run_suite(base, log)
	local fails = 0
	local function record(ok, msg)
		if not ok then
			fails = fails + 1
		end
		log[#log + 1] = (ok and "ok   " or "FAIL ") .. msg
	end

	local palette = palette_for()
	if #palette < 2 then
		record(false, "need at least 2 normal nodes registered to test with")
		return fails
	end

	local stride = math.max(PS.x, PS.z) + 4

	-- Every section writes into its own slot along +Z, and the snapshot has to
	-- cover all of them or the suite leaves test builds in the world. Each
	-- placement calls claim(), which fails the run if it would land outside
	-- the snapshot, so adding a section cannot silently leak again.
	local Z_CONFLICT = stride * 2 -- 5 deep
	local Z_IMPORT = 27           -- staff builder from JSON, 13 deep
	local Z_CHEST = 45            -- one node
	local Z_MTS = 50              -- staff builder from .mts, 13 deep
	local Z_PROBE = 66            -- keep-existing probe, one deep

	local area_p1 = vector.subtract(base, vector.new(2, 2, 2))
	local area_p2 = vector.add(base, vector.new(stride * 5 + 4, 8, 72))

	local function claim(p1, p2, what)
		local inside = p1.x >= area_p1.x and p1.y >= area_p1.y
			and p1.z >= area_p1.z and p2.x <= area_p2.x
			and p2.y <= area_p2.y and p2.z <= area_p2.z
		if not inside then
			record(false, what .. " writes outside the restored test volume " ..
				core.pos_to_string(p1) .. ".." .. core.pos_to_string(p2))
		end
		return inside
	end

	core.load_area(area_p1, area_p2)
	local snap = snapshot(area_p1, area_p2)

	local ok, err = pcall(function()
		-- Source pattern.
		for z = 0, PS.z - 1 do
			for y = 0, PS.y - 1 do
				for x = 0, PS.x - 1 do
					core.set_node(vector.add(base, vector.new(x, y, z)),
						{name = pattern_node(palette, x, y, z)})
				end
			end
		end

		local schem = gs.copy(TEST_PLAYER, base,
			vector.add(base, vector.new(PS.x - 1, PS.y - 1, PS.z - 1)))

		-- 1. Rotation fidelity against the engine itself.
		for slot, rot in ipairs(gs.ROTATIONS) do
			local dest = vector.add(base, vector.new(stride * slot, 0, 0))
			claim(dest, vector.add(dest, vector.new(stride - 1, PS.y - 1,
				stride - 1)), "rotation test")
			if core.place_schematic(dest, schem, rot, nil, true) == nil then
				record(false, "rot " .. rot .. ": place_schematic returned nil")
			else
				local predicted = gs.rotate(schem, rot)
				local size = predicted.size
				local bad, first = 0, nil
				for z = 0, size.z - 1 do
					for y = 0, size.y - 1 do
						for x = 0, size.x - 1 do
							local want = gs.get(predicted, x, y, z).name
							local got = core.get_node(
								vector.add(dest, vector.new(x, y, z))).name
							if got ~= want then
								bad = bad + 1
								first = first or string.format(
									"(%d,%d,%d) map=%s preview=%s",
									x, y, z, got, want)
							end
						end
					end
				end
				local n = size.x * size.y * size.z
				record(bad == 0, string.format(
					"rot %-3s preview matches place_schematic (%d nodes, %dx%dx%d)%s",
					rot, n, size.x, size.y, size.z,
					bad > 0 and (" - " .. bad .. " differ, " .. first) or ""))
			end
		end

		-- 2. Conflict classification, on a deliberately occupied destination.
		local size = schem.size
		local dest = vector.add(base, vector.new(0, 0, Z_CONFLICT))
		claim(dest, vector.add(dest, vector.new(size.x - 1, size.y - 1,
			size.z - 1)), "conflict test")
		for z = 0, size.z - 1 do
			for y = 0, size.y - 1 do
				for x = 0, size.x - 1 do
					core.set_node(vector.add(dest, vector.new(x, y, z)),
						{name = palette[1]})
				end
			end
		end
		local before = read_region(dest, size)

		local pv = gs.show(TEST_PLAYER, schem, dest, "0", true)
		local shown = pv.counts.shown
		record(pv.counts.overwrite == shown and pv.counts.skipped == 0,
			string.format("force=on: %d/%d reported as overwritten",
				pv.counts.overwrite, shown))

		pv.force = false
		pv:refresh()
		record(pv.counts.skipped == shown and pv.counts.overwrite == 0,
			string.format("force=off: %d/%d reported as skipped",
				pv.counts.skipped, shown))

		-- 3. Commit, then undo.
		pv.force = true
		pv:refresh()
		local committed, result = gs.commit(TEST_PLAYER)
		record(committed, "commit succeeded" ..
			(committed and "" or (" - " .. tostring(result))))

		if committed then
			local expected = {}
			for z = 0, size.z - 1 do
				for y = 0, size.y - 1 do
					for x = 0, size.x - 1 do
						expected[#expected + 1] = gs.get(schem, x, y, z).name
					end
				end
			end
			local diff = first_difference(read_region(dest, size), expected)
			record(diff == nil, "commit wrote the schematic to the map" ..
				(diff and (" - differs at entry " .. diff) or ""))
			record(gs.get_preview(TEST_PLAYER) == nil,
				"preview cleared after commit")

			local undone, uerr = gs.undo(TEST_PLAYER)
			record(undone, "undo succeeded" ..
				(undone and "" or (" - " .. tostring(uerr))))
			if undone then
				local diff2 = first_difference(read_region(dest, size), before)
				record(diff2 == nil, "undo restored the region exactly" ..
					(diff2 and (" - differs at entry " .. diff2) or ""))
			end
		end

		-- 4. Outline fallback above the entity budget.
		local saved = gs.settings.max_entities
		gs.settings.max_entities = 1
		local big = gs.show(TEST_PLAYER, schem, dest, "0", true)
		record(big.outlined and #big.objects == 12, string.format(
			"oversized schematic falls back to an outline (outlined=%s, %d beams)",
			tostring(big.outlined), #big.objects))

		-- Counting beams is not enough: they also have to be on the edges of
		-- the bounding box. Each edge beam runs along one axis, so exactly two
		-- of its three coordinates sit at a box extreme and the third sits at
		-- the centre of its axis.
		local p1b, p2b = big:bounds()
		local lo = vector.subtract(p1b, 0.5)
		local hi = vector.add(p2b, 0.5)
		local seen, misplaced = {}, 0
		for _, obj in ipairs(big.objects) do
			local pos = obj:get_pos()
			local extremes, centres = 0, 0
			for _, axis in ipairs({"x", "y", "z"}) do
				local mid = (lo[axis] + hi[axis]) / 2
				if math.abs(pos[axis] - lo[axis]) < 1e-4
						or math.abs(pos[axis] - hi[axis]) < 1e-4 then
					extremes = extremes + 1
				elseif math.abs(pos[axis] - mid) < 1e-4 then
					centres = centres + 1
				end
			end
			if not (extremes == 2 and centres == 1) then
				misplaced = misplaced + 1
			end
			seen[core.pos_to_string(pos)] = true
		end
		local distinct = 0
		for _ in pairs(seen) do distinct = distinct + 1 end
		record(misplaced == 0 and distinct == 12, string.format(
			"outline beams lie on the 12 box edges (%d misplaced, %d distinct positions)",
			misplaced, distinct))

		gs.hide(TEST_PLAYER)
		gs.settings.max_entities = saved

		-- 5. File round trip. This path shipped broken once because nothing
		-- exercised it, so it is covered now: export, list, read back, compare.
		local fname = "ghostschem_selftest_tmp"
		local exported, path = gs.save_file(TEST_PLAYER, fname)
		record(exported, "export clipboard to .mts" ..
			(exported and "" or (" - " .. tostring(path))))

		if exported then
			local listed = false
			for _, n in ipairs(gs.list_files()) do
				if n == fname then
					listed = true
				end
			end
			record(listed, "exported schematic appears in the file listing")

			local reloaded, reread = gs.load_file(TEST_PLAYER, fname)
			record(reloaded, "read the .mts back" ..
				(reloaded and "" or (" - " .. tostring(reread))))

			if reloaded then
				local a, b = schem.size, reread.size
				local same_size = a.x == b.x and a.y == b.y and a.z == b.z
				local bad = 0
				if same_size then
					for z = 0, a.z - 1 do
						for y = 0, a.y - 1 do
							for x = 0, a.x - 1 do
								if gs.get(schem, x, y, z).name
										~= gs.get(reread, x, y, z).name then
									bad = bad + 1
								end
							end
						end
					end
				end
				record(same_size and bad == 0, string.format(
					"round-tripped .mts matches the original (%dx%dx%d, %d differ)",
					b.x, b.y, b.z, bad))
			end

			pcall(os.remove, path)
		end

		-- 6. Import the reference JSON from an external builder.
		local fixture = core.get_modpath("ghostschem") ..
			DIR_DELIM .. "tests" .. DIR_DELIM .. "fixtures" ..
			DIR_DELIM .. "staff-builder.schematic.json"
		local text = gs.read_file(fixture)
		record(text ~= nil, "read the reference JSON fixture")

		if text then
			local imported, iinfo = gs.import_json(text)
			record(imported ~= nil, "import the reference JSON" ..
				(imported and "" or (" - " .. tostring(iinfo))))

			if imported then
				local sz = imported.size
				record(sz.x == 14 and sz.y == 5 and sz.z == 13, string.format(
					"imported bounding box is 14x5x13 (got %dx%dx%d)",
					sz.x, sz.y, sz.z))
				record(iinfo.filled == 235, string.format(
					"imported 235 listed nodes (got %d)", iinfo.filled))
				record(iinfo.stacks == 3, string.format(
					"carried 3 item stacks (got %d)", iinfo.stacks))
				record(iinfo.duplicates == 0, string.format(
					"no duplicate positions (got %d)", iinfo.duplicates))
				record(gs.count_stacks(imported) == 3,
					"stacks are on the node entries")

				-- place_schematic must tolerate the extra `stack` field that
				-- rides along on node entries; read_schematic_def only reads
				-- name, param1/prob, param2 and force_place.
				local far = vector.add(base, vector.new(0, 0, Z_IMPORT))
				claim(far, vector.add(far, vector.new(sz.x - 1, sz.y - 1,
					sz.z - 1)), "JSON import placement")
				core.load_area(far, vector.add(far,
					vector.new(sz.x, sz.y, sz.z)))
				record(core.place_schematic(far, imported, "0", nil, true)
					~= nil, "place_schematic accepts entries carrying stacks")

				-- A preview of it must build without error even though this
				-- game has none of the nc_* node types registered.
				local ipv = gs.show(TEST_PLAYER, imported, far, "90", true)
				record(ipv ~= nil and ipv.counts.total == 235, string.format(
					"preview of the import counts 235 nodes (got %d)",
					ipv and ipv.counts.total or -1))
				record(ipv.counts.stacks == 3, string.format(
					"rotated preview still reports 3 stacks (got %d)",
					ipv.counts.stacks))
				gs.hide(TEST_PLAYER)

				-- The user-facing path: drop the .json into the world's
				-- schems directory and load it by bare name.
				local dir = core.get_worldpath() .. DIR_DELIM .. "schems"
				core.mkdir(dir)
				local dropped = dir .. DIR_DELIM .. "gs_import_tmp.json"
				local out = io.open(dropped, "wb")
				if out then
					out:write(text)
					out:close()

					local lok, lschem, linfo =
						gs.load_file(TEST_PLAYER, "gs_import_tmp")
					record(lok, "/gs load finds a .json by name" ..
						(lok and "" or (" - " .. tostring(lschem))))
					if lok then
						record(linfo ~= nil and linfo.filled == 235,
							"loading a .json returns its import info")
						local seen = false
						for _, n in ipairs(gs.list_files()) do
							if n == "gs_import_tmp" then seen = true end
						end
						record(seen, ".json appears in the file listing")
					end
					pcall(os.remove, dropped)
				end
			end
		end

		-- 7. Writing an item stack into a node that really has an inventory.
		--    This is the part placement cannot do by itself.
		if core.registered_nodes["chest:chest"] then
			local spot = vector.add(base, vector.new(0, 0, Z_CHEST))
			claim(spot, spot, "item stack test")
			core.load_area(spot, vector.add(spot, vector.new(1, 1, 1)))
			core.set_node(spot, {name = "air"})

			local with_stack = {
				size = {x = 1, y = 1, z = 1},
				data = {{
					name = "chest:chest",
					param2 = 0,
					prob = 255,
					stack = {name = "default:stick", count = 7},
				}},
			}
			-- Use a real item from this game so the stack is known.
			for item in pairs(core.registered_items) do
				if item ~= "" and not core.registered_nodes[item] then
					with_stack.data[1].stack.name = item
					break
				end
			end

			core.place_schematic(spot, with_stack, "0", nil, true)

			-- Straight after the blit the chest has no inventory at all,
			-- because placeOnMap never runs on_construct.
			local lists_before = core.get_meta(spot):get_inventory():get_lists()
			local n_before = 0
			for _ in pairs(lists_before) do n_before = n_before + 1 end
			record(n_before == 0,
				"schematic placement leaves a container unconstructed " ..
				"(no inventory lists)")

			local applied, sfailed = gs.apply_stacks(spot, with_stack)
			record(applied == 1 and #sfailed == 0, string.format(
				"apply_stacks constructs the node and writes the stack " ..
				"(%d applied, %d failed%s)", applied, #sfailed,
				sfailed[1] and (": " .. sfailed[1].why) or ""))

			if applied == 1 then
				local inv = core.get_meta(spot):get_inventory()
				local wanted = with_stack.data[1].stack
				record(inv:contains_item("main",
					ItemStack(wanted.name .. " " .. wanted.count)),
					"the item really is in the container inventory")
			end
		end

		-- 8. .mts files written by tools/json2mts.js. The engine's own reader
		--    must see exactly what the in-game JSON importer sees.
		local fixtures = core.get_modpath("ghostschem") .. DIR_DELIM ..
			"tests" .. DIR_DELIM .. "fixtures" .. DIR_DELIM

		local function same_as_import(label, mts, json, unlisted_prob)
			local from_mts = core.read_schematic(fixtures .. mts, {})
			local from_json = gs.import_json(gs.read_file(fixtures .. json) or "")
			if not (from_mts and from_json) then
				record(false, label .. ": could not read " ..
					(from_mts and json or mts))
				return
			end
			local a, b = from_mts.size, from_json.size
			if a.x ~= b.x or a.y ~= b.y or a.z ~= b.z then
				record(false, string.format("%s: size %dx%dx%d, import says %dx%dx%d",
					label, a.x, a.y, a.z, b.x, b.y, b.z))
				return
			end
			local bad, first = 0, nil
			for i = 1, a.x * a.y * a.z do
				local m, j = from_mts.data[i], from_json.data[i]
				-- read_schematic reports probability doubled: 0x7F -> 254.
				local want = (j.name == "air") and unlisted_prob or 254
				if m.name ~= j.name or m.param2 ~= j.param2 or m.prob ~= want then
					bad = bad + 1
					first = first or string.format("cell %d: %s/%d/%d vs %s/%d/%d",
						i, m.name, m.param2, m.prob, j.name, j.param2, want)
				end
			end
			record(bad == 0, string.format(
				"%s: engine reads all %d cells as the JSON import does%s",
				label, a.x * a.y * a.z,
				first and (" - " .. bad .. " differ, " .. first) or ""))
		end

		same_as_import("json2mts staff-builder.mts", "staff-builder.mts",
			"staff-builder.schematic.json", 254)
		same_as_import("json2mts --keep-existing", "keep-existing.keep.mts",
			"keep-existing.schematic.json", 0)

		-- The converted file must place like the preview says it will.
		local mts_at = vector.add(base, vector.new(0, 0, Z_MTS))
		claim(mts_at, vector.add(mts_at, vector.new(13, 4, 12)),
			"converted .mts placement")
		record(core.place_schematic(mts_at, fixtures .. "staff-builder.mts",
			"0", nil, true) ~= nil, "place_schematic loads the converted .mts")

		-- And --keep-existing must actually leave the world alone in the gaps.
		for i, variant in ipairs({"air", "keep"}) do
			local at = vector.add(base, vector.new((i - 1) * 6, 0, Z_PROBE))
			claim(at, vector.add(at, vector.new(2, 0, 0)), "probe placement")
			for x = 0, 2 do
				core.set_node(vector.add(at, vector.new(x, 0, 0)),
					{name = palette[1]})
			end
			core.place_schematic(at,
				fixtures .. "keep-existing." .. variant .. ".mts", "0", nil, true)
			local gap = core.get_node(vector.add(at, vector.new(1, 0, 0))).name
			local want = variant == "air" and "air" or palette[1]
			record(gap == want, string.format(
				"json2mts %s mode: unlisted gap holds %s (want %s)",
				variant == "air" and "default" or "--keep-existing", gap, want))
		end
	end)

	restore(snap)
	gs.hide(TEST_PLAYER)
	gs.undo_stack[TEST_PLAYER] = nil

	-- The suite promises not to damage the world, so check it kept that
	-- promise: the restored volume must match the snapshot exactly.
	local after = snapshot(area_p1, area_p2)
	local changed = 0
	for i = 1, #snap.data do
		if after.data[i] ~= snap.data[i] or after.param2[i] ~= snap.param2[i] then
			changed = changed + 1
		end
	end
	record(changed == 0 and #after.data == #snap.data, string.format(
		"test volume restored exactly (%d of %d nodes differ)",
		changed, #snap.data))

	if not ok then
		record(false, "suite crashed: " .. tostring(err))
	end
	return fails
end

--------------------------------------------------------------------------
-- 5. Emerge-and-reclassify, which is inherently asynchronous
--------------------------------------------------------------------------

local function run_emerge_check(base, done)
	-- Somewhere far enough out that it has almost certainly never been
	-- generated, so the preview must emerge it before it can classify.
	local far = vector.add(base, vector.new(4096, 0, 4096))
	local schem = {
		size = {x = 2, y = 1, z = 2},
		data = {},
	}
	for i = 1, 4 do
		schem.data[i] = {name = "air", prob = 255}
	end
	-- One real node, so there is something to classify.
	schem.data[1] = {name = (palette_for()[1] or "air"), prob = 255}

	local pv = gs.show(TEST_PLAYER, schem, far, "0", true)
	local was_unknown = pv.counts.unknown

	core.after(4, function()
		local now = gs.get_preview(TEST_PLAYER)
		local unknown_now = now and now.counts.unknown or -1
		gs.hide(TEST_PLAYER)
		if was_unknown == 0 then
			-- Already generated; nothing to prove, and not a failure.
			done(0, "skip ungenerated-area check (region already generated)")
		elseif unknown_now == 0 then
			done(0, string.format(
				"ok   preview emerged ungenerated map and reclassified (%d -> 0)",
				was_unknown))
		else
			done(1, string.format(
				"FAIL preview did not reclassify after emerge (%d -> %d)",
				was_unknown, unknown_now))
		end
	end)
end

--------------------------------------------------------------------------
-- Entry points
--------------------------------------------------------------------------

local function report(fails, log)
	local header = fails == 0
		and ("Ghost schematic self test: all %d checks passed"):format(#log)
		or ("Ghost schematic self test: %d of %d checks FAILED"):format(fails, #log)
	return header .. "\n  " .. table.concat(log, "\n  ")
end

function gs.selftest(player_name, on_done)
	local player = core.get_player_by_name(player_name)
	if not player then
		return false, "you must be in-game"
	end
	local base = vector.round(vector.add(player:get_pos(), vector.new(0, 8, 0)))

	local log = {}
	local fails = run_suite(base, log)

	run_emerge_check(base, function(extra_fails, line)
		fails = fails + extra_fails
		log[#log + 1] = line
		local text = report(fails, log)
		if on_done then
			on_done(fails == 0, text)
		else
			core.chat_send_player(player_name, text)
		end
	end)

	return true, "Running self test, results in a moment..."
end

if core.settings:get_bool("ghostschem_selftest_on_start", false) then
	core.after(0.5, function()
		local base = vector.new(0, 8, 0)
		core.emerge_area(base, vector.add(base, vector.new(64, 8, 64)),
			function(_, _, remaining)
				if remaining ~= 0 then
					return
				end
				local log = {}
				local fails = run_suite(base, log)
				run_emerge_check(base, function(extra, line)
					fails = fails + extra
					log[#log + 1] = line
					print(report(fails, log))
					print("GHOSTSCHEM SELFTEST " ..
						(fails == 0 and "PASS" or "FAIL"))
					core.request_shutdown("selftest finished")
				end)
			end)
	end)
end
