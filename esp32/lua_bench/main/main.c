// Runs bench.lua on Luanti's bundled Lua 5.1 and prints timings, once per
// allocator strategy. Build one variant per number type (double, float,
// LNUM int32/int64) to compare them; see ../CMakeLists.txt.
// Needed to see Lua's internal structs (TValue/Node sizes); LNUM keeps their
// configuration in luaconf_internal.h, which lua.h only includes for core files.
#define LUA_CORE
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lobject.h"

extern const char bench_lua_start[] asm("_binary_bench_lua_start");
extern const char bench_lua_end[] asm("_binary_bench_lua_end");

/* ---- Allocators ------------------------------------------------------- */

// 1. Stock: what luaL_newstate() does (plain realloc). With
//    CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=256, small blocks prefer internal RAM.
static void *alloc_stock(void *ud, void *ptr, size_t osize, size_t nsize)
{
	if (nsize == 0) {
		free(ptr);
		return NULL;
	}
	return realloc(ptr, nsize);
}

// 2. Everything in PSRAM: what small objects get once internal RAM is used up
static void *alloc_psram(void *ud, void *ptr, size_t osize, size_t nsize)
{
	if (nsize == 0) {
		heap_caps_free(ptr);
		return NULL;
	}
	return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// 3/4. Small-object pool. Lua always tells the allocator the old block size,
// so blocks need no header: the size class comes from osize. Blocks up to
// POOL_MAX bytes come from per-class free lists carved out of 16 KB slabs;
// bigger ones go to the normal heap.
#define POOL_MAX 128
#define POOL_GRAIN 8
#define POOL_CLASSES (POOL_MAX / POOL_GRAIN)
#define SLAB_SIZE (16 * 1024)

typedef struct pool {
	void *free_list[POOL_CLASSES];
	uint8_t *slab_pos, *slab_end;
	size_t internal_budget; // bytes of slabs still allowed in internal RAM
	unsigned slabs_internal, slabs_psram;
} pool_t;

static inline unsigned size_class(size_t n)
{
	return (n + POOL_GRAIN - 1) / POOL_GRAIN - 1;
}

static void *pool_get(pool_t *p, size_t n)
{
	unsigned c = size_class(n);
	void *b = p->free_list[c];
	if (b) {
		p->free_list[c] = *(void **)b;
		return b;
	}
	size_t sz = (c + 1) * POOL_GRAIN;
	if (p->slab_pos + sz > p->slab_end) {
		uint8_t *slab = NULL;
		if (p->internal_budget >= SLAB_SIZE) {
			slab = heap_caps_aligned_alloc(8, SLAB_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
			if (slab) {
				p->internal_budget -= SLAB_SIZE;
				p->slabs_internal++;
			}
		}
		if (!slab) {
			slab = heap_caps_aligned_alloc(8, SLAB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
			if (!slab)
				return NULL;
			p->slabs_psram++;
		}
		// The unused tail of the previous slab is simply abandoned
		p->slab_pos = slab;
		p->slab_end = slab + SLAB_SIZE;
	}
	b = p->slab_pos;
	p->slab_pos += sz;
	return b;
}

static void pool_put(pool_t *p, void *b, size_t n)
{
	unsigned c = size_class(n);
	*(void **)b = p->free_list[c];
	p->free_list[c] = b;
}

static void *alloc_pool(void *ud, void *ptr, size_t osize, size_t nsize)
{
	pool_t *p = ud;
	bool old_small = ptr && osize <= POOL_MAX;
	if (nsize == 0) {
		if (old_small)
			pool_put(p, ptr, osize);
		else
			free(ptr);
		return NULL;
	}
	if (nsize <= POOL_MAX) {
		if (old_small && size_class(osize) == size_class(nsize))
			return ptr;
		void *b = pool_get(p, nsize);
		if (b && ptr) {
			memcpy(b, ptr, osize < nsize ? osize : nsize);
			if (old_small)
				pool_put(p, ptr, osize);
			else
				free(ptr);
		}
		return b;
	}
	if (old_small) {
		void *b = malloc(nsize);
		if (b) {
			memcpy(b, ptr, osize);
			pool_put(p, ptr, osize);
		}
		return b;
	}
	return realloc(ptr, nsize); // ptr may be NULL: plain malloc
}

/* ---- Lua helpers ------------------------------------------------------ */

// timeit(fn) -> elapsed_us, fn() result. Timed in C so the float build's
// clock never needs large Lua numbers.
static int l_timeit(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_pushvalue(L, 1);
	int64_t t0 = esp_timer_get_time();
	lua_call(L, 0, 1);
	int64_t dt = esp_timer_get_time() - t0;
	lua_pushnumber(L, (lua_Number)dt);
	lua_insert(L, -2);
	return 2;
}

// sysfree_kb() -> free KB in internal RAM + PSRAM
static int l_sysfree_kb(lua_State *L)
{
	lua_pushnumber(L, (lua_Number)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
	return 1;
}

static void open_lib(lua_State *L, lua_CFunction f, const char *name)
{
	lua_pushcfunction(L, f);
	lua_pushstring(L, name);
	lua_call(L, 1, 0);
}

static int l_panic(lua_State *L)
{
	printf("Lua panic: %s\n", lua_tostring(L, -1));
	return 0;
}

static void run(const char *mode, lua_Alloc fn, void *ud)
{
	printf("\n---- allocator: %s ----\n", mode);
	lua_State *L = lua_newstate(fn, ud);
	if (!L) {
		printf("lua_newstate failed\n");
		return;
	}
	lua_atpanic(L, l_panic);
	open_lib(L, luaopen_base, "");
	open_lib(L, luaopen_table, LUA_TABLIBNAME);
	open_lib(L, luaopen_string, LUA_STRLIBNAME);
	open_lib(L, luaopen_math, LUA_MATHLIBNAME);
	lua_register(L, "timeit", l_timeit);
	lua_register(L, "sysfree_kb", l_sysfree_kb);

	size_t len = bench_lua_end - bench_lua_start - 1; // drop EMBED_TXTFILES' NUL
	if (luaL_loadbuffer(L, bench_lua_start, len, "bench.lua") || lua_pcall(L, 0, 0, 0))
		printf("Lua error: %s\n", lua_tostring(L, -1));
	lua_close(L);
}

static void free_slabs_note(const pool_t *p)
{
	printf("pool slabs: %u internal, %u PSRAM (%u KB; not returned in this test)\n",
		p->slabs_internal, p->slabs_psram,
		(p->slabs_internal + p->slabs_psram) * SLAB_SIZE / 1024);
}

static void bench_task(void *arg)
{
	const char *variant = LUA_BENCH_VARIANT_NAME;
	printf("\n==== Lua 5.1 benchmark: variant %s ====\n", variant);
	printf("sizeof(lua_Number)=%u sizeof(TValue)=%u sizeof(Node)=%u\n",
		(unsigned)sizeof(lua_Number), (unsigned)sizeof(TValue), (unsigned)sizeof(Node));

	run("stock", alloc_stock, NULL);
	run("psram_only", alloc_psram, NULL);

	static pool_t pool_psram;
	run("pool_psram", alloc_pool, &pool_psram);
	free_slabs_note(&pool_psram);

	static pool_t pool_internal = {.internal_budget = 128 * 1024};
	run("pool_internal", alloc_pool, &pool_internal);
	free_slabs_note(&pool_internal);

	printf("Free after run: internal %u KB, PSRAM %u KB\n",
		(unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
		(unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
	printf("==== done (%s) ====\n", variant);
	vTaskDelete(NULL);
}

void app_main(void)
{
	// Lua's parser and VM recurse on the C stack
	xTaskCreatePinnedToCore(bench_task, "lua_bench", 32 * 1024, NULL, 5, NULL, 1);
}
