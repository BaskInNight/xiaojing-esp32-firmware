# Boot smoke test — minimal app to verify PSRAM + boot stability
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components
)

# No XIAOJING_TESTING — this is a pure boot test

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(boot_smoke)
