# Lua 5.1 number-type benchmark on the ESP32-S3.
# Build the stock (double) variant:   idf.py -B build_double build
# Build the float variant:           idf.py -B build_float -DLUA_BENCH_FLOAT=1 build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lua_bench)
