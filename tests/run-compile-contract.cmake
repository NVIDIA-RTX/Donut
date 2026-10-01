# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
# SPDX-License-Identifier: MIT

set(command "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${BUILD_TARGET}")
if (NOT "${BUILD_CONFIG}" STREQUAL "")
    list(APPEND command --config "${BUILD_CONFIG}")
endif()
execute_process(COMMAND ${command} RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE error)
get_filename_component(log_directory "${LOG_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${log_directory}")
file(WRITE "${LOG_FILE}" "${output}${error}")

if ("${EXPECT_DIAGNOSTIC}" STREQUAL "")
    if (NOT "${result}" STREQUAL "0")
        message(FATAL_ERROR "Positive compile control failed (${result}):\n${output}${error}")
    endif()
elseif ("${result}" STREQUAL "0")
    message(FATAL_ERROR "${BUILD_TARGET} unexpectedly compiled; its contract was not enforced.")
elseif (NOT "${output}${error}" MATCHES "${EXPECT_DIAGNOSTIC}")
    message(FATAL_ERROR "Compilation failed for an unexpected reason (${result}):\n${output}${error}")
endif()
