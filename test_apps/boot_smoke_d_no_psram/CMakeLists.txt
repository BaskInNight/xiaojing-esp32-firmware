# Boot smoke D — NO PSRAM baseline
# Isolates CPU/Flash/boot chain from PSRAM
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(boot_smoke_d)
