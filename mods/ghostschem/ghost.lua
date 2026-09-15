-- The ghost visual.
--
-- Why `visual = "cube"` and not `visual = "node"`:
--
-- The object fragment shader ends with
--     col = vec4(col.rgb, base.a);        -- object_shader/opengl_fragment.glsl
-- so an entity's output alpha comes *only* from its texture. Vertex-colour
-- alpha is already spent carrying the day/night sunlight ratio
-- (opengl_vertex.glsl: nightRatio = 1.0 - color.a; color.a = 1.0), so it
-- cannot carry opacity.
--
-- That means the only way to get a translucent entity is to bake the
-- translucency into the texture string with ^[opacity:N. And that only works
-- for visuals whose textures *are* strings:
--
--   visual = "cube"  -> textures are plain strings, and GenericCAO::
--                       updateTextures() applies set_texture_mod to them.
--                       Translucent. Shape is always a full cube.
--   visual = "node"  -> textures come from the nodedef, and updateTextures()
--                       has no OBJECTVISUAL_NODE branch at all, so
--                       set_texture_mod is silently ignored. Correct shape
--                       (nodeboxes, stairs, slabs), but always opaque.
--
-- We want translucency more than we want stair shapes, so: cube. See README
-- for the small engine patch that lifts this restriction.

local gs = ghostschem

gs.settings = {
	-- 0 = invisible, 255 = solid.
	opacity = tonumber(core.settings:get("ghostschem_opacity")) or 110,
	-- Above this many ghost entities we draw a bounding box instead.
	max_entities = tonumber(core.settings:get("ghostschem_max_entities")) or 3000,
}

--------------------------------------------------------------------------
-- Texture derivation
--------------------------------------------------------------------------
-- Cube entity face order is Up, Down, +X, -X, +Z, -Z
-- (src/client/mesh.cpp, createCubeMesh), which is exactly the node `tiles`
-- order documented as "+Y, -Y, +X, -X, +Z, -Z". So tiles map 1:1 onto the
-- entity's textures with no reordering.
--
-- When a nodedef gives fewer than 6 tiles the engine repeats the last one
-- (c_content.cpp: "Copy last value to all remaining textures"); we match that.

local VARIANT_TINT = {
	-- Will be placed into empty space.
	ok = "",
	-- Something is already there and force_placement will overwrite it.
	overwrite = "^[colorize:#ff3524:170",
	-- Something is already there and it will be skipped (force_placement off).
	skipped = "^[colorize:#ffb300:170",
	-- Target area is not loaded, so we cannot tell yet.
	unknown = "^[colorize:#8060ff:150",
}

gs.VARIANTS = {}
for variant in pairs(VARIANT_TINT) do
	gs.VARIANTS[variant] = true
end

local texture_cache = {}

function gs.flush_texture_cache()
	texture_cache = {}
end

local function tile_to_texture(tile)
	if type(tile) == "table" then
		return tile.name or tile.image
	end
	return tile
end

function gs.node_textures(name, variant)
	local key = name .. "\0" .. variant
	local cached = texture_cache[key]
	if cached then
		return cached
	end

	local def = core.registered_nodes[name]
	local tiles = def and def.tiles
	local suffix = (VARIANT_TINT[variant] or "")
		.. "^[opacity:" .. gs.settings.opacity

	local textures = {}
	for i = 1, 6 do
		local tex
		if tiles and #tiles > 0 then
			tex = tile_to_texture(tiles[i] or tiles[#tiles])
		end
		if type(tex) ~= "string" or tex == "" then
			tex = "unknown_node.png"
		end
		textures[i] = tex .. suffix
	end

	texture_cache[key] = textures
	return textures
end

--------------------------------------------------------------------------
-- Entities
--------------------------------------------------------------------------

-- Slightly larger than a node so that an "overwrite" ghost visibly encloses
-- the real node it is sitting on instead of z-fighting with it.
local GHOST_SCALE = 1.02

core.register_entity("ghostschem:ghost", {
	initial_properties = {
		visual = "cube",
		visual_size = {x = GHOST_SCALE, y = GHOST_SCALE, z = GHOST_SCALE},
		textures = {
			"unknown_node.png", "unknown_node.png", "unknown_node.png",
			"unknown_node.png", "unknown_node.png", "unknown_node.png",
		},
		physical = false,
		collide_with_objects = false,
		collisionbox = {0, 0, 0, 0, 0, 0},
		selectionbox = {0, 0, 0, 0, 0, 0},
		pointable = false,
		-- Never written to the mapblock, so max_objects_per_block (which only
		-- gates *stored* objects, see MapBlock::saveStaticObject) never applies
		-- and ghosts can never be left behind in a saved world.
		static_save = false,
		-- Selects TILE_MATERIAL_PLAIN_ALPHA in GenericCAO::updateMaterialType,
		-- i.e. real alpha blending, unlit so ghosts read the same in a cave as
		-- in daylight.
		use_texture_alpha = true,
		shaded = false,
		glow = 14,
		-- A translucent cube looks wrong with its back faces missing.
		backface_culling = false,
	},

	on_activate = function(self, staticdata)
		-- Ghosts are strictly transient. If one somehow comes back from
		-- staticdata (an old world, a crash), drop it rather than leaving an
		-- orphan floating in the map.
		if staticdata ~= "" then
			self.object:remove()
		end
	end,

	on_punch = function()
		return true -- never take damage, never drop anything
	end,
})

-- Thin beam used for the bounding-box fallback on oversized schematics.
local OUTLINE_TEX = "[fill:16x16:#66ccffc0"
core.register_entity("ghostschem:outline", {
	initial_properties = {
		visual = "cube",
		visual_size = {x = 1, y = 1, z = 1},
		-- [fill generates the texture in the engine, so the mod ships no
		-- image files at all. The alpha byte is part of the ColorString.
		textures = {
			OUTLINE_TEX, OUTLINE_TEX, OUTLINE_TEX,
			OUTLINE_TEX, OUTLINE_TEX, OUTLINE_TEX,
		},
		physical = false,
		collide_with_objects = false,
		collisionbox = {0, 0, 0, 0, 0, 0},
		selectionbox = {0, 0, 0, 0, 0, 0},
		pointable = false,
		static_save = false,
		use_texture_alpha = true,
		shaded = false,
		glow = 14,
		backface_culling = false,
	},

	on_activate = function(self, staticdata)
		if staticdata ~= "" then
			self.object:remove()
		end
	end,
})

--------------------------------------------------------------------------
-- Spawning helpers
--------------------------------------------------------------------------

function gs.spawn_ghost(pos, nodename, variant)
	local obj = core.add_entity(pos, "ghostschem:ghost")
	if not obj then
		return nil
	end
	obj:set_properties({textures = gs.node_textures(nodename, variant)})
	return obj
end

function gs.spawn_beam(pos, size, colour)
	local obj = core.add_entity(pos, "ghostschem:outline")
	if not obj then
		return nil
	end
	local tex = colour and ("[fill:16x16:" .. colour) or OUTLINE_TEX
	obj:set_properties({
		visual_size = size,
		textures = {tex, tex, tex, tex, tex, tex},
	})
	return obj
end
