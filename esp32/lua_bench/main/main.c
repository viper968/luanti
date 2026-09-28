// Runs bench.lua on Luanti's bundled Lua 5.1 and prints timings.
// Build it twice (double / float lua_Number) and compare; see ../CMakeLists.txt.
#include <stdio.h>
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

static void open_lib(lua_State *L, lua_CFunction f, const char *name)
{
	lua_pushcfunction(L, f);
	lua_pushstring(L, name);
	lua_call(L, 1, 0);
}

static void bench_task(void *arg)
{
#ifdef LUA_BENCH_FLOAT
	const char *variant = "float";
#else
	const char *variant = "double";
#endif
	printf("\n==== Lua 5.1 number benchmark: lua_Number = %s ====\n", variant);
	printf("sizeof(lua_Number)=%u sizeof(TValue)=%u sizeof(Node)=%u\n",
		(unsigned)sizeof(lua_Number), (unsigned)sizeof(TValue), (unsigned)sizeof(Node));

	lua_State *L = luaL_newstate();
	open_lib(L, luaopen_base, "");
	open_lib(L, luaopen_table, LUA_TABLIBNAME);
	open_lib(L, luaopen_string, LUA_STRLIBNAME);
	open_lib(L, luaopen_math, LUA_MATHLIBNAME);
	lua_register(L, "timeit", l_timeit);

	size_t len = bench_lua_end - bench_lua_start - 1; // drop EMBED_TXTFILES' NUL
	if (luaL_loadbuffer(L, bench_lua_start, len, "bench.lua") || lua_pcall(L, 0, 0, 0))
		printf("Lua error: %s\n", lua_tostring(L, -1));
	lua_close(L);

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
