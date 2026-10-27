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

# NVRHI's DXR cluster path is built against one preview SDK version (see nvrhi/d3d12.h).
# A preview package keeps each version in its own folder, so accept the package root too.
# The resolved folder is a normal variable, so the cached path stays as the user set it.
set(DONUT_DXRCLUSTER_AGILITY_SDK_VERSION 722)
if(NVRHI_D3D12_WITH_DXRCLUSTER AND EXISTS "${DONUT_D3D_AGILITY_SDK_PATH}/1.${DONUT_DXRCLUSTER_AGILITY_SDK_VERSION}/include/d3d12.h")
    set(DONUT_D3D_AGILITY_SDK_PATH "${DONUT_D3D_AGILITY_SDK_PATH}/1.${DONUT_DXRCLUSTER_AGILITY_SDK_VERSION}")
endif()

# find_path/find_file cache their results, so a build tree that switches to a different
# SDK would silently keep using the previously found one. Re-search when the path changes.
if(NOT "${DONUT_D3D_AGILITY_SDK_PATH}" STREQUAL "${_d3d_agility_cached_path}")
    unset(_d3d_agility_include CACHE)
    unset(DONUT_D3D_AGILITY_SDK_CORE_DLL CACHE)
    unset(DONUT_D3D_AGILITY_SDK_LAYERS_DLL CACHE)
    set(_d3d_agility_cached_path "${DONUT_D3D_AGILITY_SDK_PATH}" CACHE INTERNAL "")
endif()

find_path(_d3d_agility_include
    d3d12.h
    PATHS
        "${DONUT_D3D_AGILITY_SDK_PATH}/build/native/include" # NuGet package layout
        "${DONUT_D3D_AGILITY_SDK_PATH}/include"              # preview package version folder (e.g. 1.722/)
    NO_SYSTEM_ENVIRONMENT_PATH
)

# Select the Agility SDK binary subdirectory that matches the target architecture.
# The NuGet package ships build/native/bin/{x64,win32,arm64}; a preview package
# version folder ships {x64,arm64}/sdkbin.
include("${CMAKE_CURRENT_LIST_DIR}/../nvrhi/cmake/NvrhiTargetArch.cmake")
if (NVRHI_TARGET_ARCH STREQUAL "arm64")
    set(_d3d_agility_arch "arm64")
else()
    set(_d3d_agility_arch "x64")
endif()

find_file(DONUT_D3D_AGILITY_SDK_CORE_DLL
    D3D12Core.dll
    PATHS
        "${DONUT_D3D_AGILITY_SDK_PATH}/build/native/bin/${_d3d_agility_arch}"
        "${DONUT_D3D_AGILITY_SDK_PATH}/${_d3d_agility_arch}/sdkbin"
    NO_SYSTEM_ENVIRONMENT_PATH
)

find_file(DONUT_D3D_AGILITY_SDK_LAYERS_DLL
    d3d12SDKLayers.dll
    PATHS
        "${DONUT_D3D_AGILITY_SDK_PATH}/build/native/bin/${_d3d_agility_arch}"
        "${DONUT_D3D_AGILITY_SDK_PATH}/${_d3d_agility_arch}/sdkbin"
    NO_SYSTEM_ENVIRONMENT_PATH
)

if(_d3d_agility_include)
    
    set(DONUT_D3D_AGILITY_SDK_INCLUDE_DIR "${_d3d_agility_include}")
    
    # find the SDK version number: the NuGet package's d3d12.idl declares it as a
    # const UINT; a preview package has no .idl and declares it as a #define in
    # d3d12.h instead.
    if(EXISTS "${_d3d_agility_include}/d3d12.idl")
        file(READ "${_d3d_agility_include}/d3d12.idl" _d3d12_idl)
        # Release packages define D3D12_PREVIEW_SDK_VERSION too; only a -preview nuspec marks a preview.
        set(_d3d_agility_nuspec "${DONUT_D3D_AGILITY_SDK_PATH}/Microsoft.Direct3D.D3D12.nuspec")
        if(EXISTS "${_d3d_agility_nuspec}")
            file(STRINGS "${_d3d_agility_nuspec}" _d3d_agility_nuspec_preview REGEX "<version>[^<]*-preview</version>")
            if(_d3d_agility_nuspec_preview)
                string(REGEX MATCH "const UINT D3D12_PREVIEW_SDK_VERSION = ([0-9]+)" _preview_match ${_d3d12_idl})
                set(_d3d_agility_preview_version "${CMAKE_MATCH_1}")
            endif()
        endif()
        string(REGEX MATCH "const UINT D3D12_SDK_VERSION = ([0-9]+)" _match ${_d3d12_idl})
    else()
        file(READ "${_d3d_agility_include}/d3d12.h" _d3d12_h)
        string(REGEX MATCH "#define[ \t]+D3D12_PREVIEW_SDK_VERSION[ \t]+\\(?[ \t]*([0-9]+)[ \t]*\\)?" _preview_match ${_d3d12_h})
        set(_d3d_agility_preview_version "${CMAKE_MATCH_1}")
        string(REGEX MATCH "#define[ \t]+D3D12_SDK_VERSION[ \t]+\\(?[ \t]*([0-9]+)[ \t]*\\)?" _match ${_d3d12_h})
    endif()
    if(_match AND CMAKE_MATCH_1)
        set(DONUT_D3D_AGILITY_SDK_VERSION ${CMAKE_MATCH_1})

        # A preview SDK ships a D3D12Core.dll stamped with its preview version rather than
        # D3D12_SDK_VERSION, and that is the number the app must export as D3D12SDKVersion:
        # exporting the release number instead fails device creation with
        # DXGI_ERROR_SDK_COMPONENT_MISSING.
        if(_d3d_agility_preview_version)
            set(DONUT_D3D_AGILITY_SDK_VERSION ${_d3d_agility_preview_version})
            set(DONUT_D3D_AGILITY_SDK_PREVIEW TRUE)
        endif()

        if(DONUT_D3D_AGILITY_SDK_PREVIEW)
            message(STATUS "Found D3D12 Agility SDK: ${DONUT_D3D_AGILITY_SDK_INCLUDE_DIR} (preview version ${DONUT_D3D_AGILITY_SDK_VERSION})")
        else()
            message(STATUS "Found D3D12 Agility SDK: ${DONUT_D3D_AGILITY_SDK_INCLUDE_DIR} (version ${DONUT_D3D_AGILITY_SDK_VERSION})")
        endif()
    else()
        message(FATAL_ERROR "Cannot resolve D3D12 Agility SDK version number")
    endif()
endif()

if(NVRHI_D3D12_WITH_DXRCLUSTER AND NOT (DONUT_D3D_AGILITY_SDK_PREVIEW AND DONUT_D3D_AGILITY_SDK_VERSION EQUAL DONUT_DXRCLUSTER_AGILITY_SDK_VERSION))
    message(FATAL_ERROR "NVRHI_D3D12_WITH_DXRCLUSTER requires exactly the 1.${DONUT_DXRCLUSTER_AGILITY_SDK_VERSION} preview D3D12 Agility SDK "
                        "(later previews are not guaranteed to work with NVIDIA driver 621.01), "
                        "but DONUT_D3D_AGILITY_SDK_PATH (${DONUT_D3D_AGILITY_SDK_PATH}) is version '${DONUT_D3D_AGILITY_SDK_VERSION}'. "
                        "Fetch the 1.${DONUT_DXRCLUSTER_AGILITY_SDK_VERSION}.0-preview NuGet package, or point it at the extracted preview package or its 1.${DONUT_DXRCLUSTER_AGILITY_SDK_VERSION} folder.")
endif()

# NVRHI compiles its DXR cluster path against these headers, so they must come from the SDK
# whose D3D12Core.dll the application ships. Only a value this script set is updated.
if(NVRHI_D3D12_WITH_DXRCLUSTER)
    if(NOT NVRHI_D3D12_DXRCLUSTER_INCLUDE_DIR OR NVRHI_D3D12_DXRCLUSTER_INCLUDE_DIR STREQUAL _donut_dxrcluster_include_dir)
        set(NVRHI_D3D12_DXRCLUSTER_INCLUDE_DIR "${DONUT_D3D_AGILITY_SDK_INCLUDE_DIR}"
            CACHE PATH "Include directory of the preview Agility SDK providing the DXR cluster headers" FORCE)
        set(_donut_dxrcluster_include_dir "${DONUT_D3D_AGILITY_SDK_INCLUDE_DIR}" CACHE INTERNAL "")
    elseif(NOT NVRHI_D3D12_DXRCLUSTER_INCLUDE_DIR STREQUAL DONUT_D3D_AGILITY_SDK_INCLUDE_DIR)
        message(FATAL_ERROR "NVRHI_D3D12_DXRCLUSTER_INCLUDE_DIR (${NVRHI_D3D12_DXRCLUSTER_INCLUDE_DIR}) does not match the Agility SDK in "
                            "DONUT_D3D_AGILITY_SDK_PATH (${DONUT_D3D_AGILITY_SDK_INCLUDE_DIR}). Clear it to use the latter.")
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
