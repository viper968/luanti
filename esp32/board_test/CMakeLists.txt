# Board bring-up test for running luantiserver on the
# Waveshare ESP32-S3-Touch-LCD-2.8B. See doc/esp32s3_server_port.md.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(luanti_board_test)
