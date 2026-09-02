# Top-level CMakeLists for the ESP-IDF build.
# These three lines (in this order) are required by ESP-IDF.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-power-monitor)
