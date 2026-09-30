#
# Copyright (c) 2014-2026, NVIDIA CORPORATION. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.


file(GLOB donut_engine_tests src/engine/test_*.cpp)

foreach(test_src ${donut_engine_tests})

    get_filename_component(test_name "${test_src}" NAME_WE)
    #message(STATUS "Added test ${test_name}")

    add_executable("${test_name}" "${test_src}")
    target_link_libraries("${test_name}" donut_app donut_engine donut_core donut_tests_utils)

    add_dependencies(donut_all_tests "${test_name}")

    add_test("${test_name}" "${test_name}")

    set_property(TARGET "${test_name}" PROPERTY FOLDER "Donut/donut_tests/donut_engine_tests")

endforeach()

# Functions are globally visible, but the Vulkan register-offset defaults are
# directory-scoped. Initialize them here as well as in the production shaders target.
include(${CMAKE_CURRENT_LIST_DIR}/../compileshaders.cmake)

donut_compile_shaders_all_platforms(
    TARGET donut_test_texcoord_shaders
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/shaders/Texcoords.cfg
    FOLDER Donut/donut_tests
    OUTPUT_BASE ${CMAKE_CURRENT_BINARY_DIR}/shaders
    OUTPUT_FORMAT BINARY
    SHADERMAKE_OPTIONS_DXIL "--hlsl2021"
    SHADERMAKE_OPTIONS_SPIRV "--hlsl2021"
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/shaders/texcoords_cs.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/shaders/texcoord_raster.hlsl)
add_dependencies(test_texcoords donut_test_texcoord_shaders)
target_compile_definitions(test_texcoords PRIVATE DONUT_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
add_dependencies(test_texcoord_raster donut_test_texcoord_shaders)
target_link_libraries(test_texcoord_raster donut_render)
set_tests_properties(test_texcoord_raster PROPERTIES SKIP_RETURN_CODE 77)

