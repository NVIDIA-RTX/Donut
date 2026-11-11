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


project(donut)
cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set_property( GLOBAL PROPERTY USE_FOLDERS ON)

if (MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /W3")
    if (NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        string(APPEND CMAKE_CXX_FLAGS " /MP")
    endif()
endif()

if (CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^(x64|x86_64)$")
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        string(APPEND CMAKE_CXX_FLAGS " -mf16c")
    endif()
endif()

if (NOT MSVC)
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " -D_DEBUG")
endif()

include(CMakeDependentOption)
include(FetchContent)

option(DONUT_WITH_NVRHI "Enable NVRHI and related projects" ON)

cmake_dependent_option(DONUT_WITH_DX11 "Enable the DX11 version of Donut" ON "WIN32" OFF)
cmake_dependent_option(DONUT_WITH_DX12 "Enable the DX12 version of Donut" ON "WIN32" OFF)
option(DONUT_WITH_VULKAN "Enable the Vulkan version of Donut" ON)
option(DONUT_WITH_AFTERMATH "Enable Aftermath crash dump generation with Donut" OFF)
option(DONUT_EMBED_SHADER_PDBS "Embed shader PDBs with shader binary files" OFF)

option(DONUT_WITH_STATIC_SHADERS "Build Donut with statically linked shaders" OFF)

option(DONUT_WITH_AUDIO "Include Audio features (XAudio2)" OFF)
option(DONUT_WITH_TINYEXR "Include TinyEXR" ON)
option(DONUT_WITH_UNIT_TESTS "Donut unit-tests (see CMake/CTest documentation)" OFF)

option(DONUT_WITH_STREAMLINE "Enable Streamline, fetched automatically unless DONUT_STREAMLINE_SEARCH_PATHS is set" OFF)
set(DONUT_STREAMLINE_SEARCH_PATHS "" CACHE STRING "Search paths for a pre-installed Streamline package")

# The integration tracks Streamline's headers, so default to the version it was tested with.
# Arm64 ships as a separate package.
include("${CMAKE_CURRENT_SOURCE_DIR}/nvrhi/cmake/NvrhiTargetArch.cmake")
if (NVRHI_TARGET_ARCH STREQUAL "arm64")
    set(_donut_sl_url "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1-aarch64.zip")
    set(_donut_sl_sha256 "d51031bcdec88d482b6e113253e17d1f31cba47f683c0e2d7e6333578d4d3869")
    set(_donut_sl_arch "arm64")
else()
    set(_donut_sl_url "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip")
    set(_donut_sl_sha256 "92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b")
    set(_donut_sl_arch "x64")
endif()
if (DONUT_STREAMLINE_SEARCH_PATHS)
    set(_donut_sl_url "")
    set(_donut_sl_sha256 "")
endif()
if (NOT DEFINED DONUT_STREAMLINE_FETCH_URL)
    set(DONUT_STREAMLINE_FETCH_URL "${_donut_sl_url}")
endif()
if (NOT DEFINED DONUT_STREAMLINE_FETCH_SHA256)
    set(DONUT_STREAMLINE_FETCH_SHA256 "${_donut_sl_sha256}")
endif()
set(STREAMLINE_TARGET_ARCH "${_donut_sl_arch}" CACHE STRING "Streamline bin/lib subdirectory to use")
set(DONUT_STREAMLINE_FETCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/streamline/${_donut_sl_arch}" CACHE STRING "Directory to fetch Streamline to, empty uses build directory default")

option(DONUT_WITH_DLSS "Enable the NGX DLSS integration, separate package required" OFF)
set(DONUT_DLSS_FETCH_URL "https://github.com/NVIDIA/DLSS.git" CACHE STRING "URL of the DLSS Git repository to fetch from")
set(DONUT_DLSS_FETCH_TAG "374959484e79a640feaba44c93ac8cfb0a03f5b5" CACHE STRING "Git commit hash or tag for DLSS") # v310.9.1, first SDK with Windows Arm64 binaries
set(DONUT_DLSS_FETCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/DLSS" CACHE STRING "Directory to fetch DLSS to, empty uses build directory default")

option(DONUT_WITH_KTX "Build KTX2 (BCn + Zstandard) texture loading support" ON)
set(DONUT_ZSTD_FETCH_URL "https://github.com/facebook/zstd.git" CACHE STRING "URL of the Zstandard Git repository to fetch from")
set(DONUT_ZSTD_FETCH_TAG "v1.5.6" CACHE STRING "Git commit hash or tag for Zstandard")
set(DONUT_ZSTD_FETCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/${NVRHI_TARGET_ARCH}" CACHE STRING "Directory to fetch Zstandard to, empty uses build directory default")

if (WIN32)
    option(DONUT_FORCE_DISCRETE_GPU "Declare symbols to make the OS run the app on discrete GPU on laptops" OFF)
endif()

add_subdirectory(thirdparty)

if (NOT TARGET ShaderMake)
    option(SHADERMAKE_FIND_FXC "" "${DONUT_WITH_DX11}")
    if (DONUT_WITH_DX12 OR DONUT_WITH_VULKAN)
        # Ask ShaderMake to download a build of DXC if we're building for DX12 or Vulkan,
        # to have something in case Vulkan SDK is not installed.
        option(SHADERMAKE_FIND_DXC "" ON)
    else()
        option(SHADERMAKE_FIND_DXC "" OFF)
    endif()
    option(SHADERMAKE_FIND_DXC_VK "" "${DONUT_WITH_VULKAN}")
    if (NOT NVRHI_HOST_ARCH STREQUAL NVRHI_TARGET_ARCH)
        option(SHADERMAKE_TOOL "" ON)
    endif()

    add_subdirectory(ShaderMake)
endif()

if (DONUT_WITH_NVRHI)
    set(NVRHI_WITH_DX11 "${DONUT_WITH_DX11}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX12 "${DONUT_WITH_DX12}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VULKAN "${DONUT_WITH_VULKAN}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_AFTERMATH "${DONUT_WITH_AFTERMATH}" CACHE BOOL "" FORCE)

    add_subdirectory(nvrhi)

    set(DONUT_SHADER_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/include CACHE PATH "" FORCE)
    
    add_subdirectory(shaders)
endif()

if (DONUT_WITH_DLSS)
    message(STATUS "Updating DLSS from " ${DONUT_DLSS_FETCH_URL} ", tag " ${DONUT_DLSS_FETCH_TAG} ", into folder " ${DONUT_DLSS_FETCH_DIR} " - please wait!" )
    FetchContent_Declare(
        DLSS
        GIT_REPOSITORY ${DONUT_DLSS_FETCH_URL}
        GIT_TAG ${DONUT_DLSS_FETCH_TAG}
        SOURCE_DIR ${DONUT_DLSS_FETCH_DIR}
    )
    FetchContent_MakeAvailable(DLSS)

    include(cmake/DLSS.cmake)
endif()

include(donut-core.cmake)
if (DONUT_WITH_NVRHI)
    include(donut-engine.cmake)
    include(donut-render.cmake)
    include(donut-app.cmake)
endif()

if (DONUT_WITH_UNIT_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()

if (DONUT_WITH_STREAMLINE)
    # Validate that CMAKE_RUNTIME_OUTPUT_DIRECTORY is set.
    # The Streamline CMake script uses it to copy DLLs, and it will fail at compile time with obscure messages
    # if the variable is empty.
    if (NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        message(SEND_ERROR "Streamline integration requires that CMAKE_RUNTIME_OUTPUT_DIRECTORY is non-empty.")
    endif()
    
    if (DONUT_STREAMLINE_FETCH_TAG)
        message(WARNING "DONUT_STREAMLINE_FETCH_TAG is deprecated. Use DONUT_STREAMLINE_FETCH_SHA256 instead. Tag fetching from the public repo no longer has the binaries.")
    endif()

    # XOR logic to make sure both options are set 
    if ((DONUT_STREAMLINE_FETCH_URL AND NOT DONUT_STREAMLINE_FETCH_SHA256) OR 
        (NOT DONUT_STREAMLINE_FETCH_URL AND DONUT_STREAMLINE_FETCH_SHA256))
        message(SEND_ERROR "Both DONUT_STREAMLINE_FETCH_URL and DONUT_STREAMLINE_FETCH_SHA256 must be defined together")
    endif()

    if (DONUT_STREAMLINE_FETCH_URL AND DONUT_STREAMLINE_FETCH_SHA256)
        message(STATUS "Updating Streamline from " ${DONUT_STREAMLINE_FETCH_URL} ", SHA256 " ${DONUT_STREAMLINE_FETCH_SHA256} " - please wait!" )
        FetchContent_Declare(
            streamline
            URL ${DONUT_STREAMLINE_FETCH_URL}
            URL_HASH SHA256=${DONUT_STREAMLINE_FETCH_SHA256}
            SOURCE_DIR ${DONUT_STREAMLINE_FETCH_DIR}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(streamline)
    else ()
        if (DONUT_STREAMLINE_SEARCH_PATHS)
            set(STREAMLINE_SEARCH_PATHS ${DONUT_STREAMLINE_SEARCH_PATHS})
        endif()
        find_package(STREAMLINE REQUIRED)

        if (STREAMLINE_FOUND)
            add_subdirectory(${STREAMLINE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/streamline)
        endif()
    endif()
endif()
