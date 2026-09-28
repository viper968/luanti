# Builds Luanti's bundled Lua 5.1.5 (lib/lua) from a copy in the build tree, so
# benchmark variants can patch it without touching lib/lua.
#
#   LUA_BENCH_VARIANT = double      stock Lua (default)
#                       float       lua_Number = float, single-precision libm
#                       lnum_int32  LNUM patch: int32 integers + double
#                       lnum_int64  LNUM patch: int64 integers + double
if(NOT LUA_BENCH_VARIANT)
	set(LUA_BENCH_VARIANT double)
endif()

set(lua_src_dir "${CMAKE_CURRENT_LIST_DIR}/../../../../lib/lua/src")
get_filename_component(lua_src_dir "${lua_src_dir}" ABSOLUTE)
set(lua_copy "${CMAKE_BINARY_DIR}/lua51_src")

# Start from a pristine copy every configure, so patches never stack up
file(REMOVE_RECURSE "${lua_copy}")
file(GLOB lua_files "${lua_src_dir}/*.c" "${lua_src_dir}/*.h")
file(COPY ${lua_files} DESTINATION "${lua_copy}")

function(patch_file path from to)
	file(READ "${path}" text)
	string(FIND "${text}" "${from}" found)
	if(found EQUAL -1)
		message(FATAL_ERROR "lua51 patch: '${from}' not found in ${path}")
	endif()
	string(REPLACE "${from}" "${to}" text "${text}")
	file(WRITE "${path}" "${text}")
endfunction()

set(core lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
	lparser lstate lstring ltable ltm lundump lvm lzio lauxlib
	lbaselib ltablib lstrlib lmathlib)
set(defs "")

if(LUA_BENCH_VARIANT STREQUAL "float")
	set(conf "${lua_copy}/luaconf.h")
	patch_file("${conf}" "#define LUA_NUMBER_DOUBLE\n#define LUA_NUMBER	double" "#define LUA_NUMBER	float")
	patch_file("${conf}" "#define LUA_NUMBER_SCAN		\"%lf\"" "#define LUA_NUMBER_SCAN		\"%f\"")
	patch_file("${conf}" "#define LUA_NUMBER_FMT		\"%.14g\"" "#define LUA_NUMBER_FMT		\"%.7g\"")
	patch_file("${conf}" "strtod((s), (p))" "strtof((s), (p))")
	patch_file("${conf}" "((a) - floor((a)/(b))*(b))" "((a) - floorf((a)/(b))*(b))")
	patch_file("${conf}" "(pow(a,b))" "(powf(a,b))")
	# Use single-precision libm in the math library too (modf/frexp/ldexp keep double)
	patch_file("${lua_copy}/lmathlib.c" "#include <math.h>\n" "#include <math.h>
#define fabs fabsf
#define sin sinf
#define cos cosf
#define tan tanf
#define asin asinf
#define acos acosf
#define atan atanf
#define atan2 atan2f
#define sinh sinhf
#define cosh coshf
#define tanh tanhf
#define ceil ceilf
#define floor floorf
#define fmod fmodf
#define sqrt sqrtf
#define pow powf
#define exp expf
#define log logf
#define log10 log10f
")
	list(APPEND defs LUA_BENCH_FLOAT=1)
elseif(LUA_BENCH_VARIANT MATCHES "^lnum_int(32|64)$")
	find_program(PATCH_EXE patch REQUIRED)
	execute_process(
		COMMAND "${PATCH_EXE}" -p2 --batch --forward --no-backup-if-mismatch
			-i "${CMAKE_CURRENT_LIST_DIR}/lnum/lua515-lnum.patch"
		WORKING_DIRECTORY "${lua_copy}"
		RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
	if(NOT rc EQUAL 0)
		message(FATAL_ERROR "lua51: LNUM patch failed:\n${out}")
	endif()
	list(APPEND core lnum)
	list(APPEND defs LNUM_DOUBLE LNUM_INT${CMAKE_MATCH_1})
elseif(NOT LUA_BENCH_VARIANT STREQUAL "double")
	message(FATAL_ERROR "lua51: unknown LUA_BENCH_VARIANT '${LUA_BENCH_VARIANT}'")
endif()
message(STATUS "lua51: variant ${LUA_BENCH_VARIANT}")

set(srcs "")
foreach(f ${core})
	list(APPEND srcs "${lua_copy}/${f}.c")
endforeach()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS "${lua_copy}")
# Upstream Lua code; don't fail the build on its warnings
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${defs}
	LUA_BENCH_VARIANT_NAME="${LUA_BENCH_VARIANT}")
