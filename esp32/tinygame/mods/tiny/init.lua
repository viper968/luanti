-- Minimal node set for mapgen v6 plus a hand that can dig everything.

core.register_item(":", {
	type = "none",
	wield_image = "wieldhand.png",
	tool_capabilities = {
		full_punch_interval = 0.9,
		max_drop_level = 0,
		groupcaps = {
			crumbly = {times = {[1] = 1.5, [2] = 0.9, [3] = 0.5}, uses = 0},
			cracky = {times = {[1] = 3.0, [2] = 2.0, [3] = 1.2}, uses = 0},
			choppy = {times = {[1] = 2.5, [2] = 1.5, [3] = 1.0}, uses = 0},
			snappy = {times = {[3] = 0.3}, uses = 0},
		},
		damage_groups = {fleshy = 1},
	},
})

local function node(name, desc, groups, extra)
	local def = {description = desc, tiles = {"tiny_" .. name .. ".png"}, groups = groups}
	for k, v in pairs(extra or {}) do def[k] = v end
	core.register_node("tiny:" .. name, def)
end

node("stone", "Stone", {cracky = 3}, {drop = "tiny:cobble"})
node("cobble", "Cobblestone", {cracky = 3})
node("dirt", "Dirt", {crumbly = 3})
node("grass", "Dirt with Grass", {crumbly = 3}, {drop = "tiny:dirt"})
node("sand", "Sand", {crumbly = 3, falling_node = 1})
node("gravel", "Gravel", {crumbly = 2, falling_node = 1})
node("tree", "Tree Trunk", {choppy = 2}, {paramtype2 = "facedir"})
node("wood", "Wooden Planks", {choppy = 3})
node("leaves", "Leaves", {snappy = 3}, {drawtype = "allfaces_optional", paramtype = "light"})

local function liquid(kind, drawtype)
	core.register_node("tiny:water_" .. kind, {
		description = "Water",
		drawtype = drawtype,
		tiles = {"tiny_water.png"},
		special_tiles = {{name = "tiny_water.png", backface_culling = false},
			{name = "tiny_water.png", backface_culling = true}},
		paramtype = "light",
		paramtype2 = kind == "flowing" and "flowingliquid" or nil,
		walkable = false, pointable = false, diggable = false, buildable_to = true,
		drowning = 1,
		liquidtype = kind,
		liquid_alternative_flowing = "tiny:water_flowing",
		liquid_alternative_source = "tiny:water_source",
		liquid_viscosity = 1,
		groups = {water = 3, liquid = 3},
	})
end
liquid("source", "liquid")
liquid("flowing", "flowingliquid")

-- Mapgen v6 needs these; unlisted v6 aliases fall back to air/ignore.
for alias, target in pairs({
	mapgen_stone = "tiny:stone", mapgen_cobble = "tiny:cobble",
	mapgen_mossycobble = "tiny:cobble", mapgen_stair_cobble = "tiny:cobble",
	mapgen_dirt = "tiny:dirt", mapgen_dirt_with_grass = "tiny:grass",
	mapgen_sand = "tiny:sand", mapgen_gravel = "tiny:gravel",
	mapgen_desert_sand = "tiny:sand", mapgen_desert_stone = "tiny:stone",
	mapgen_water_source = "tiny:water_source", mapgen_river_water_source = "tiny:water_source",
	mapgen_lava_source = "tiny:stone",
	mapgen_tree = "tiny:tree", mapgen_leaves = "tiny:leaves", mapgen_apple = "tiny:leaves",
	mapgen_jungletree = "tiny:tree", mapgen_jungleleaves = "tiny:leaves",
	mapgen_junglegrass = "air", mapgen_pine_tree = "tiny:tree", mapgen_pine_needles = "tiny:leaves",
	mapgen_snow = "tiny:grass", mapgen_snowblock = "tiny:dirt",
	mapgen_dirt_with_snow = "tiny:grass", mapgen_ice = "tiny:water_source",
}) do
	core.register_alias(alias, target)
end

core.register_on_newplayer(function(player)
	player:get_inventory():add_item("main", "tiny:wood 99")
end)
