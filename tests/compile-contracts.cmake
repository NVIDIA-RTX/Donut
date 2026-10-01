# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
# SPDX-License-Identifier: MIT

# Build through ordinary project targets so probes inherit the configured
# compiler, platform, include paths, and definitions from donut_render.
function(donut_add_geometry_compile_test name diagnostic)
    set(target donut_compile_${name})
    add_library(${target} STATIC EXCLUDE_FROM_ALL src/compile/geometry_input_contract.cpp)
    target_link_libraries(${target} PRIVATE donut_render)
    if (ARGN)
        target_compile_definitions(${target} PRIVATE ${ARGN})
    endif()
    set_property(TARGET ${target} PROPERTY FOLDER "Donut/donut_tests/compile_contracts")
    add_test(NAME test_geometry_compile_${name}
        COMMAND ${CMAKE_COMMAND}
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DBUILD_TARGET=${target}"
            "-DBUILD_CONFIG=$<CONFIG>"
            "-DEXPECT_DIAGNOSTIC=${diagnostic}"
            "-DLOG_FILE=${CMAKE_CURRENT_BINARY_DIR}/compile-contracts/${name}-$<CONFIG>.log"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-compile-contract.cmake")
    # These tests invoke the build tool on a shared build tree.
    set_tests_properties(test_geometry_compile_${name} PROPERTIES
        RUN_SERIAL TRUE LABELS compile-contracts)
endfunction()

# Positive control includes complete configurations, every Custom input override,
# and compatible Stock pixel-shader extensions. Build it with the normal tests.
donut_add_geometry_compile_test(valid "")
add_dependencies(donut_all_tests donut_compile_valid)

set(fields PushConstants InputSpace PushConstantBinding InstanceBinding VertexBinding
    Visibility HasNormals HasPrevPosition)
set(index 0)
foreach(field IN LISTS fields)
    math(EXPR index "${index} + 1")
    donut_add_geometry_compile_test(missing_${field}
        "Geometry input configuration must declare" OMIT_CONFIG_FIELD=${index})
endforeach()

set(pass_index 0)
foreach(pass Depth Forward GBuffer)
    set(hook_index 0)
    foreach(hook VertexShader InputLayout FormatInputLayout BindingLayout BindingSet)
        math(EXPR hook_index "${hook_index} + 1")
        donut_add_geometry_compile_test(stock_${pass}_${hook} "error[^\n]*final"
            STOCK_PASS=${pass_index} OVERRIDE_INPUT_HOOK=${hook_index})
    endforeach()
    math(EXPR pass_index "${pass_index} + 1")
endforeach()
