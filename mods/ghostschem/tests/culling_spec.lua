core = {registered_nodes = {
	["default:stone"] = {drawtype = "normal"},
	["default:dirt"]  = {},                                  -- drawtype defaults to normal
	["default:glass"] = {drawtype = "glasslike"},
	["default:leaves"]= {drawtype = "allfaces_optional"},
	["default:water"] = {drawtype = "liquid", use_texture_alpha = "blend"},
}}
vector = {}
ghostschem = {}
dofile("/home/user/luanti/mods/ghostschem/api.lua")
local gs = ghostschem

local function build(sx, sy, sz, fn)
	local size = {x = sx, y = sy, z = sz}
	local schem = {size = size, data = {}}
	for z = 0, sz - 1 do for y = 0, sy - 1 do for x = 0, sx - 1 do
		schem.data[gs.index(size, x, y, z)] = {name = fn(x, y, z), prob = 255}
	end end end
	return schem
end

local failed = 0
local function check(label, got, want)
	local ok = got == want
	if not ok then failed = failed + 1 end
	print(string.format("%-46s %6s (got %d, want %d)",
		label, ok and "ok" or "FAIL", got, want))
end

-- 1. Solid opaque cube: only the shell should be drawn.
local solid = build(16, 16, 16, function() return "default:stone" end)
local shown, total = gs.each_visible(solid, function() end)
check("solid 16^3 stone: total nodes", total, 4096)
check("solid 16^3 stone: ghosts drawn", shown, 16*16*16 - 14*14*14)

-- 2. Air is never a ghost and never occludes.
local half = build(4, 4, 4, function(_, y) return y < 2 and "default:stone" or "air" end)
local shown2, total2 = gs.each_visible(half, function() end)
check("half-air 4^3: total nodes", total2, 32)
check("half-air 4^3: ghosts drawn (none enclosed)", shown2, 32)

-- 3. Transparent nodes must never be culled, and never cull their neighbours.
local glassy = build(5, 5, 5, function(x, y, z)
	local interior = x > 0 and x < 4 and y > 0 and y < 4 and z > 0 and z < 4
	return interior and "default:stone" or "default:glass"
end)
local shown3 = gs.each_visible(glassy, function() end)
-- The 3^3 stone core is wrapped in glass, which is not opaque, so the outer
-- layer of that core stays visible. Only the single centre node is enclosed
-- ... except its neighbours are stone, so it IS enclosed. 5^3 - 1 = 124.
check("glass shell around stone: ghosts drawn", shown3, 124)

-- 4. Liquids are not opaque (use_texture_alpha = blend), so no culling.
local water = build(4, 4, 4, function() return "default:water" end)
local shown4 = gs.each_visible(water, function() end)
check("solid water 4^3: ghosts drawn", shown4, 64)

-- 5. Unregistered nodes stay visible rather than vanishing silently.
local unknown = build(3, 3, 3, function() return "nosuchmod:nosuchnode" end)
local shown5 = gs.each_visible(unknown, function() end)
check("unknown nodes 3^3: ghosts drawn", shown5, 27)

-- 6. prob = 0 nodes are never placed, so never previewed.
local probz = build(3, 3, 3, function(x) return "default:stone" end)
for i = 1, 27 do probz.data[i].prob = 0 end
local shown6, total6 = gs.each_visible(probz, function() end)
check("prob=0 nodes: total counted", total6, 0)
check("prob=0 nodes: ghosts drawn", shown6, 0)

print(failed == 0 and "\nall culling checks passed" or "\nFAILURES: " .. failed)
os.exit(failed == 0 and 0 or 1)
