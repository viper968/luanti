# Lua 5.1 number-type and allocator benchmark on the ESP32-S3.
# One build directory per variant (double, float, lnum_int32, lnum_int64):
#   idf.py -B build_<v> -DLUA_BENCH_VARIANT=<v> -DSDKCONFIG=build_<v>/sdkconfig build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lua_bench)
