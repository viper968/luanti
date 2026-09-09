local builtin_shared = ...
local debug_getinfo = debug.getinfo

-- One-element table shared with the C++ side, which reads the mod origin of the
-- running callback back out of it on demand (see CUSTOM_RIDX_LAST_RUN_MOD).
-- Storing into it directly is a plain table write, whereas core.set_last_run_mod()
-- is a call across the C API boundary -- which run_callbacks() would otherwise
-- pay for every single callback of every single event, and which also stops
-- LuaJIT from compiling the dispatch loop below.
-- The fallback only applies to pure-Lua test harnesses, where there is no
-- C++ side to keep in sync in the first place.
local last_run_mod_cell = core.get_last_run_mod_cell and core.get_last_run_mod_cell() or {}
core.get_last_run_mod_cell = nil

do
	local default = {mod = "??", name = "??"}
	core.callback_origins = setmetatable({}, {
		__index = function()
			return default
		end
	})
end

function core.run_callbacks(callbacks, mode, ...)
	assert(type(callbacks) == "table")
	local cb_len = #callbacks
	if cb_len == 0 then
		if mode == 2 or mode == 3 then
			return true
		elseif mode == 4 or mode == 5 then
			return false
		end
	end
	local ret = nil
	for i = 1, cb_len do
		local origin = core.callback_origins[callbacks[i]]
		last_run_mod_cell[1] = origin.mod
		local cb_ret = callbacks[i](...)

		if mode == 0 and i == 1 then
			ret = cb_ret
		elseif mode == 1 and i == cb_len then
			ret = cb_ret
		elseif mode == 2 then
			if not cb_ret or i == 1 then
				ret = cb_ret
			end
		elseif mode == 3 then
			if cb_ret then
				return cb_ret
			end
			ret = cb_ret
		elseif mode == 4 then
			if (cb_ret and not ret) or i == 1 then
				ret = cb_ret
			end
		elseif mode == 5 and cb_ret then
			return cb_ret
		end
	end
	return ret
end

function builtin_shared.make_registration()
	local t = {}
	local registerfunc = function(func)
		t[#t + 1] = func
		core.callback_origins[func] = {
			-- may be nil or return nil
			mod = core.get_current_modname and core.get_current_modname() or "??",
			name = debug_getinfo(1, "n").name or "??"
		}
	end
	return t, registerfunc
end

function builtin_shared.make_registration_reverse()
	local t = {}
	local registerfunc = function(func)
		table.insert(t, 1, func)
		core.callback_origins[func] = {
			-- may be nil or return nil
			mod = core.get_current_modname and core.get_current_modname() or "??",
			name = debug_getinfo(1, "n").name or "??"
		}
	end
	return t, registerfunc
end
