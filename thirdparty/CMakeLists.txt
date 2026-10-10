#
# Copyright (c) 2014-2025, NVIDIA CORPORATION. All rights reserved.
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


# Required dependencies
set(third_party_folder "Third-Party Libraries")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if (DONUT_WITH_NVRHI AND NOT TARGET imgui)
    include(imgui.cmake)
    set_target_properties(imgui PROPERTIES FOLDER ${third_party_folder})
endif()

if (NOT TARGET jsoncpp_static)
    add_library(jsoncpp_static)
    set(jsoncpp_path "${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp-amalgam")
    target_include_directories(jsoncpp_static PUBLIC "${jsoncpp_path}")
    target_sources(jsoncpp_static PRIVATE "${jsoncpp_path}/jsoncpp.cpp")
    set_target_properties(jsoncpp_static PROPERTIES FOLDER ${third_party_folder})
endif()

if (DONUT_WITH_KTX AND NOT TARGET libzstd_static)
    message(STATUS "Updating Zstandard from " ${DONUT_ZSTD_FETCH_URL} ", tag " ${DONUT_ZSTD_FETCH_TAG} ", into folder " ${DONUT_ZSTD_FETCH_DIR} " - please wait!" )
    # Leave ZSTD_USE_STATIC_RUNTIME off: it rewrites /MD to /MT in CMAKE_*_FLAGS,
    # while the CRT actually comes from CMAKE_MSVC_RUNTIME_LIBRARY, which zstd inherits.
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY ${DONUT_ZSTD_FETCH_URL}
        GIT_TAG ${DONUT_ZSTD_FETCH_TAG}
        SOURCE_DIR ${DONUT_ZSTD_FETCH_DIR}
        SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(zstd)
    set_target_properties(libzstd_static PROPERTIES FOLDER ${third_party_folder})
    # zstd installs its headers, .lib and cmake/pkgconfig files, which would
    # otherwise land in a consuming project's package as include/ and lib/.
    option(DONUT_ZSTD_INSTALL "Install zstd headers, library and cmake package" OFF)
    if (NOT DONUT_ZSTD_INSTALL)
        set_property(DIRECTORY "${zstd_SOURCE_DIR}/build/cmake" PROPERTY EXCLUDE_FROM_ALL YES)
    endif()
endif()

if (NOT TARGET stb)
    add_library(stb INTERFACE)
    target_include_directories(stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)
endif()

if (NOT TARGET cgltf)
    add_library(cgltf INTERFACE)
    target_include_directories(cgltf INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cgltf)
endif()

# Optional dependencies

if (DONUT_WITH_TINYEXR AND NOT TARGET tinyexr)
    add_library(tinyexr INTERFACE)
    target_include_directories(tinyexr INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tinyexr)
endif()

if (DONUT_WITH_NVRHI AND NOT TARGET glfw)
    option(GLFW_BUILD_EXAMPLES "" OFF)
    option(GLFW_BUILD_TESTS "" OFF)
    option(GLFW_BUILD_DOCS "" OFF)
    option(GLFW_INSTALL "" OFF)
    add_subdirectory(glfw)
    set_target_properties(glfw PROPERTIES FOLDER ${third_party_folder})
    set_target_properties(update_mappings PROPERTIES FOLDER ${third_party_folder})
endif()
