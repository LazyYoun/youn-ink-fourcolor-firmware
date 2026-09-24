# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "6.6.0")

# Local project components under main/components
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/main/components")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_LIST_DIR}/compat/cxx_math_compat.h>")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)
