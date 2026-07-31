#
# Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

#
# Helper script to enable support for D3D12's Agility SDK
#
# * This script can be configured to use either a given path location or
#   a URL to a download server ; automated fetch from Microsfot's NuGet
#   repository is the recommended method.
#   ex:
#      set(DONUT_D3D_AGILITY_SDK_URL "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.614.1")
#      include("${PROJECT_SOURCE_DIR}/extern/donut/cmake/FetchAgilitySDK.cmake")
#
# * The client application is responsible for managing the placement of
#   the Agility SDK DLLs, include paths and compile definitions.
#   ex:
#      if(DONUT_D3D_AGILITY_SDK_ENABLED)
#          target_compile_definitions(${app} PUBLIC DONUT_D3D_AGILITY_SDK_ENABLED=1)
#          target_compile_definitions(${app} PUBLIC DONUT_D3D_AGILITY_SDK_VERSION=${DONUT_D3D_AGILITY_SDK_VERSION})
#          add_custom_command(TARGET ${app} POST_BUILD
#              COMMAND 
#                  ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${app}>/D3D12/"
#              COMMAND 
#                  ${CMAKE_COMMAND} -E copy_if_different ${DONUT_D3D_AGILITY_SDK_CORE_DLL} "$<TARGET_FILE_DIR:${app}>/D3D12/"
#          )
#      endif()
#
# * The client application is also responsible for getting D3D12 to
#   dynamically load the Agility SDK DLLs.
#   ex: 
#      #ifdef DONUT_D3D_AGILITY_SDK_ENABLED
#          _declspec(dllexport) extern const UINT D3D12SDKVersion = DONUT_D3D_AGILITY_SDK_VERSION;
#          _declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
#      #endif
#
# see: https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
#

if(DONUT_D3D_AGILITY_SDK_PATH AND DONUT_D3D_AGILITY_SDK_URL)
    message(FATAL_ERROR "both DONUT_D3D_AGILITY_SDK_PATH and DONUT_D3D_AGILITY_SDK_URL were set: pick one")
endif()

set(DONUT_D3D_AGILITY_SDK_FETCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/agility_sdk" CACHE STRING "Directory to fetch the D3D12 Agility SDK to, empty uses build directory default")

if(DONUT_D3D_AGILITY_SDK_URL)

    # example :
    #     set(DONUT_D3D_AGILITY_SDK_URL "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.614.1")

    include(FetchContent)

    if(DONUT_D3D_AGILITY_SDK_FETCH_DIR)
        FetchContent_Declare(d3d_agility_sdk 
            URL "${DONUT_D3D_AGILITY_SDK_URL}"
            SOURCE_DIR "${DONUT_D3D_AGILITY_SDK_FETCH_DIR}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        set(DONUT_D3D_AGILITY_SDK_PATH "${DONUT_D3D_AGILITY_SDK_FETCH_DIR}")
    else()
        FetchContent_Declare(d3d_agility_sdk 
            URL "${DONUT_D3D_AGILITY_SDK_URL}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        set(DONUT_D3D_AGILITY_SDK_PATH "${CMAKE_BINARY_DIR}/_deps/d3d_agility_sdk-src/")        
    endif()

    FetchContent_MakeAvailable(d3d_agility_sdk)

endif()

#
#
#

find_path(_d3d_agility_include
    d3d12.h
    PATHS
        "${DONUT_D3D_AGILITY_SDK_PATH}/build/native/include"
    NO_SYSTEM_ENVIRONMENT_PATH
)

# Select the Agility SDK binary subdirectory that matches the target architecture.
# The NuGet package ships build/native/bin/{x64,win32,arm64}.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64" OR CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64")
    set(_d3d_agility_arch "arm64")
else()
    set(_d3d_agility_arch "x64")
endif()

find_file(DONUT_D3D_AGILITY_SDK_CORE_DLL
    D3D12Core.dll
    PATHS
        "${DONUT_D3D_AGILITY_SDK_PATH}/build/native/bin/${_d3d_agility_arch}"
    NO_SYSTEM_ENVIRONMENT_PATH
)

find_file(DONUT_D3D_AGILITY_SDK_LAYERS_DLL
    d3d12SDKLayers.dll
    PATHS
        "${DONUT_D3D_AGILITY_SDK_PATH}/build/native/bin/${_d3d_agility_arch}"
    NO_SYSTEM_ENVIRONMENT_PATH
)

if(_d3d_agility_include)
    
    set(DONUT_D3D_AGILITY_SDK_INCLUDE_DIR "${_d3d_agility_include}")
    
    # find the SDK version number
    file(READ "${_d3d_agility_include}/d3d12.idl" _d3d12_idl)
    string(REGEX MATCH "const UINT D3D12_SDK_VERSION = ([0-9]+)" _match ${_d3d12_idl})
    if(_match AND CMAKE_MATCH_1)
        set(DONUT_D3D_AGILITY_SDK_VERSION ${CMAKE_MATCH_1})
        message(STATUS "Found D3D12 Agility SDK: ${DONUT_D3D_AGILITY_SDK_INCLUDE_DIR} (version ${DONUT_D3D_AGILITY_SDK_VERSION})")
    else()
        message(FATAL_ERROR "Cannot resolve D3D12 Agility SDK version number")
    endif()    
endif()

#
# note : we cannot use CMake IMPLIB here because the SDK DLL is dynamically loadead and
# it doesn't provide .lib files ; CMake client-code will be responsible for moving the
# DLL to the correct location. 
#
# ex:
# add_custom_command(TARGET ${app} POST_BUILD
#     COMMAND 
#         ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${app}>/D3D12/"
#     COMMAND 
#         ${CMAKE_COMMAND} -E copy_if_different ${DONUT_D3D_AGILITY_SDK_CORE_DLL} "$<TARGET_FILE_DIR:${app}>/D3D12/")
# 

if(DONUT_D3D_AGILITY_SDK_CORE_DLL AND DONUT_D3D_AGILITY_SDK_LAYERS_DLL)
    set(DONUT_D3D_AGILITY_SDK_LIBRARIES "${DONUT_D3D_AGILITY_SDK_CORE_DLL}" "${DONUT_D3D_AGILITY_SDK_LAYERS_DLL}")
endif()

if(DONUT_D3D_AGILITY_SDK_INCLUDE_DIR AND DONUT_D3D_AGILITY_SDK_LIBRARIES AND DONUT_D3D_AGILITY_SDK_VERSION)
    set(DONUT_D3D_AGILITY_SDK_ENABLED TRUE)
endif()
