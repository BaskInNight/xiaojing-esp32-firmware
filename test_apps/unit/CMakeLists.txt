# 小净 Phase 1 单元测试应用
cmake_minimum_required(VERSION 3.16)

# Point to project's components directory
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components
)

# Enable test-only APIs across all components
add_compile_definitions(XIAOJING_TESTING=1)

# Wake engines are hardware/model integration components.  Unit tests compile
# voice_frontend and inject backend ops fakes, so do not resolve or link the
# Edge Impulse SDK or the ESP-SR managed component here.
set(EXCLUDE_COMPONENTS voice_wake_edge voice_wake_esp_sr)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaojing_unit_test)
