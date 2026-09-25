-- Ghost Schematics
--
-- A schematic system that previews placement with translucent "ghost blocks"
-- instead of writing to the map and relying on undo afterwards.
--
-- See README.md for the engine constraints this design works within.

ghostschem = {}

local modpath = core.get_modpath("ghostschem")

dofile(modpath .. "/api.lua")
dofile(modpath .. "/ghost.lua")
dofile(modpath .. "/formats.lua")
dofile(modpath .. "/preview.lua")
dofile(modpath .. "/place.lua")
dofile(modpath .. "/tool.lua")
dofile(modpath .. "/commands.lua")

-- The in-engine self test registers a few marker nodes, so it is opt-in and
-- off by default. Enable with ghostschem_selftest = true, then run /gs selftest.
if core.settings:get_bool("ghostschem_selftest", false) then
	dofile(modpath .. "/tests/ingame.lua")
end
