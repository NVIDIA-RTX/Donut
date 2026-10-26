#
# Copyright (c) 2014-2024, NVIDIA CORPORATION. All rights reserved.
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

set (NVRHI_DEFAULT_VK_REGISTER_OFFSETS
    --tRegShift 0
    --sRegShift 128
    --bRegShift 256
    --uRegShift 384)

# Generates a build target that will compile shaders for a given config file
# Usage:
#
# donut_compile_shaders(TARGET <generated build target name>
#                       CONFIG <shader-config-file>
#                       SOURCES <list>
#                       [FOLDER <folder-in-visual-studio-solution>]
#                       [OUTPUT_FORMAT (HEADER|BINARY)]
#                       [DXIL <dxil-output-path>]
#                       [DXIL_SLANG <dxil-output-path>]
#                       [DXBC <dxbc-output-path>]
#                       [SPIRV_DXC <spirv-output-path>]
#                       [SPIRV_SLANG <spirv-output-path>]
#                       [SHADERMAKE_OPTIONS <string>]       -- arguments passed to ShaderMake
#                       [SHADERMAKE_OPTIONS_DXBC <string>]  -- same, only DXBC specific
#                       [SHADERMAKE_OPTIONS_DXIL <string>]  -- same, only DXIL specific
#                       [SHADERMAKE_OPTIONS_SPIRV <string>] -- same, only SPIR-V specific
#                       [SHADER_MODEL <string>]             -- shader model n_n format (default: 6_5)
#                       [BYPRODUCTS_DXBC <list>]            -- list of generated files without paths,
#                       [BYPRODUCTS_DXIL <list>]               needed to get correct incremental builds when
#                       [BYPRODUCTS_SPIRV <list>]              using static shaders with Ninja generator
#                       [INCLUDES <list>]                   -- include paths
#                       [IGNORE_INCLUDES <list>])           -- list of included files for ShaderMake to ignore (e.g. c++)

function(donut_compile_shaders)
    set(options "")
    set(oneValueArgs
        SHADERMAKE_OPTIONS
        SHADERMAKE_OPTIONS_DXBC
        SHADERMAKE_OPTIONS_DXIL
        SHADERMAKE_OPTIONS_SPIRV
        COMPILER_OPTIONS        # deprecated
        COMPILER_OPTIONS_DXBC   # deprecated
        COMPILER_OPTIONS_DXIL   # deprecated
        COMPILER_OPTIONS_SPIRV  # deprecated
        SHADER_MODEL
        CONFIG
        DXBC
        DXIL
        DXIL_SLANG
        FOLDER
        OUTPUT_FORMAT
        PROJECT_NAME
        SPIRV_DXC
        SPIRV_SLANG
        VULKAN_VERSION
        TARGET)
    set(multiValueArgs
        BYPRODUCTS_DXBC
        BYPRODUCTS_DXIL
        BYPRODUCTS_SPIRV
        SOURCES
        INCLUDES
        IGNORE_INCLUDES
        RELAXED_INCLUDES)       # deprecated
    cmake_parse_arguments(params "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT params_TARGET)
        message(FATAL_ERROR "donut_compile_shaders: TARGET argument missing")
    endif()
    if (NOT params_CONFIG)
        message(FATAL_ERROR "donut_compile_shaders: CONFIG argument missing")
    endif()
    if ((params_DXIL AND params_DXIL_SLANG) OR (params_SPIRV AND params_SPIRV_SLANG))
        message(FATAL_ERROR "donut_compile_shaders: DXIL and DXIL_SLANG, or SPIRV and SPIRV_SLANG cannot be used together")
    endif()
    if (params_COMPILER_OPTIONS OR params_COMPILER_OPTIONS_DXBC OR
        params_COMPILER_OPTIONS_DXIL OR params_COMPILER_OPTIONS_SPIRV)
        message(SEND_ERROR "donut_compile_shaders: The COMPILER_OPTIONS[_platform] arguments "
                           "are deprecated, use SHADERMAKE_OPTIONS[_platform] instead")
    endif()
    if (params_RELAXED_INCLUDES)
        message(SEND_ERROR "donut_compile_shaders: The RELAXED_INCLUDES argument "
                           "is deprecated, use IGNORE_INCLUDES instead")
    endif()

    if (params_VULKAN_VERSION)
        set(VULKAN_VERSION ${params_VULKAN_VERSION})
    else()
        set(VULKAN_VERSION 1.2)
    endif()

    if (NOT TARGET ${params_TARGET})
        # just add the source files to the project as documents, they are built by the script
        set_source_files_properties(${params_SOURCES} PROPERTIES VS_TOOL_OVERRIDE "None") 

        add_custom_target(${params_TARGET}
            DEPENDS ${SHADERMAKE_PATH}
            SOURCES ${params_SOURCES})
    endif()
	
	if (NOT params_SHADER_MODEL)
		set(params_SHADER_MODEL "6_5")
	endif()

    if ("${params_OUTPUT_FORMAT}" STREQUAL "HEADER")
        set(output_format_arg --headerBlob)
    elseif(("${params_OUTPUT_FORMAT}" STREQUAL "BINARY") OR ("${params_OUTPUT_FORMAT}" STREQUAL ""))
        set(output_format_arg --binaryBlob --outputExt .bin)
    else()
        message(FATAL_ERROR "donut_compile_shaders: unsupported OUTPUT_FORMAT = '${params_OUTPUT_FORMAT}'")
    endif()

    if (params_PROJECT_NAME)
        set(project_name_arg --project "${params_PROJECT_NAME}")
    else()
        set(project_name_arg "")
    endif()

    separate_arguments(params_SHADERMAKE_OPTIONS       NATIVE_COMMAND "${params_SHADERMAKE_OPTIONS}")
    separate_arguments(params_SHADERMAKE_OPTIONS_DXIL  NATIVE_COMMAND "${params_SHADERMAKE_OPTIONS_DXIL}")
    separate_arguments(params_SHADERMAKE_OPTIONS_DXBC  NATIVE_COMMAND "${params_SHADERMAKE_OPTIONS_DXBC}")
    separate_arguments(params_SHADERMAKE_OPTIONS_SPIRV NATIVE_COMMAND "${params_SHADERMAKE_OPTIONS_SPIRV}")

    
    set(include_dirs "")
    set(ignore_includes "")

    # Loop over each path and append it with '-I ' prefix
    foreach(include_dir ${DONUT_SHADER_INCLUDE_DIR})
        set(include_dirs ${include_dirs} -I "${include_dir}")
    endforeach()
    
	foreach(include_dir ${params_INCLUDES})
		set(include_dirs ${include_dirs} -I "${include_dir}")
	endforeach()    

    foreach(include_file ${params_IGNORE_INCLUDES})
        set(ignore_includes ${ignore_includes} --relaxedInclude "${include_file}")
    endforeach()

    # Lets shader configs add the DXR cluster variants that cluster_args.hlsli describes.
    set(dxil_defines "")
    if (NVRHI_D3D12_WITH_DXRCLUSTER)
        set(dxil_defines -D NVRHI_D3D12_WITH_DXRCLUSTER=1)
    endif()

    if (params_DXIL AND DONUT_WITH_DX12)
        if (NOT EXISTS "${SHADERMAKE_DXC_PATH}")
            message(FATAL_ERROR "donut_compile_shaders: DXC not found -- please set SHADERMAKE_DXC_PATH to the full path to the DXC binary")
        endif()
        
        set(compilerCommand ${SHADERMAKE_PATH}
           --config ${params_CONFIG}
           --out ${params_DXIL}
           --platform DXIL
           ${output_format_arg}
           ${include_dirs}
           ${ignore_includes}
           -D TARGET_D3D12
           ${dxil_defines}
           --compiler "${SHADERMAKE_DXC_PATH}"
           --shaderModel ${params_SHADER_MODEL}
           ${project_name_arg})

        list(APPEND compilerCommand ${params_SHADERMAKE_OPTIONS})
        list(APPEND compilerCommand ${params_SHADERMAKE_OPTIONS_DXIL})

        if ("${params_BYPRODUCTS_DXIL}" STREQUAL "")
            add_custom_command(TARGET ${params_TARGET} PRE_BUILD COMMAND ${compilerCommand})
        else()
            set(byproducts_with_paths "")
            foreach(relative_path IN LISTS params_BYPRODUCTS_DXIL)
                list(APPEND byproducts_with_paths "${params_DXIL}/${relative_path}")
            endforeach()
            
            add_custom_command(TARGET ${params_TARGET} PRE_BUILD COMMAND ${compilerCommand} BYPRODUCTS "${byproducts_with_paths}")
        endif()
    endif()

    if (params_DXIL_SLANG AND DONUT_WITH_DX12)
        if (NOT EXISTS "${SHADERMAKE_SLANG_PATH}")
            message(FATAL_ERROR "donut_compile_shaders: Slang not found --- please set SHADERMAKE_SLANG_PATH to the full path to the Slang executable")
        endif()
        
        set(compilerCommand ${SHADERMAKE_PATH}
           --config ${params_CONFIG}
           --out ${params_DXIL_SLANG}
           --platform DXIL
           ${output_format_arg}
           ${include_dirs}
           ${ignore_includes}
           -D TARGET_D3D12
           ${dxil_defines}
           --compiler "${SHADERMAKE_SLANG_PATH}"
           --slang
           --shaderModel ${params_SHADER_MODEL}
           ${project_name_arg})

        list(APPEND compilerCommand ${params_SHADERMAKE_OPTIONS})
        list(APPEND compilerCommand ${params_SHADERMAKE_OPTIONS_DXIL})

        if ("${params_BYPRODUCTS_DXIL}" STREQUAL "")
            add_custom_command(TARGET ${params_TARGET} PRE_BUILD COMMAND ${compilerCommand})
        else()
            set(byproducts_with_paths "")
            foreach(relative_path IN LISTS params_BYPRODUCTS_DXIL)
                list(APPEND byproducts_with_paths "${params_DXIL_SLANG}/${relative_path}")
            endforeach()
            
            add_custom_command(TARGET ${params_TARGET} PRE_BUILD COMMAND ${compilerCommand} BYPRODUCTS "${byproducts_with_paths}")
        endif()
    endif()

    if (params_DXBC AND DONUT_WITH_DX11)
        if (NOT EXISTS "${SHADERMAKE_FXC_PATH}")
            message(FATAL_ERROR "donut_compile_shaders: FXC not found -- please set SHADERMAKE_FXC_PATH to the full path to the FXC binary")
        endif()
        
        set(compilerCommand ${SHADERMAKE_PATH}
           --config ${params_CONFIG}
           --out ${params_DXBC}
           --platform DXBC
           ${output_format_arg}
           ${include_dirs}
           ${ignore_includes}
           -D TARGET_D3D11
           --compiler "${SHADERMAKE_FXC_PATH}"
           ${project_name_arg})

        list(APPEND compilerCommand ${params_SHADERMAKE_OPTIONS})
        list(APPEND compilerCommand ${params_SHADERMAKE_OPTIONS_DXBC})

        if ("${params_BYPRODUCTS_DXBC}" STREQUAL "")
            add_custom_command(TARGET ${params_TARGET} PRE_BUILD COMMAND ${compilerCommand})
        else()
            set(byproducts_with_paths "")
            foreach(relative_path IN LISTS params_BYPRODUCTS_DXBC)
                list(APPEND byproducts_with_paths "${params_DXBC}/${relative_path}")
            endforeach()

            add_custom_command(TARGET ${params_TARGET} PRE_BUILD COMMAND ${compilerCommand} BYPRODUCTS "${byproducts_with_paths}")
        endif()
    endif()

    if (params_SPIRV_DXC AND DONUT_WITH_VULKAN)
        if (NOT EXISTS "${SHADERMAKE_DXC_VK_PATH}")
            message(FATAL_ERROR "donut_compile_shaders: DXC not found -- please set SHADERMAKE_DXC_VK_PATH to the full path to the DXC binary")
        endif()
        
        set(compilerCommand ${SHADERMAKE_PATH}
           --config ${params_CONFIG}
           --out ${params_SPIRV_DXC}
           --platform SPIRV
           ${output_format_arg}
           ${include_dirs}
           ${ignore_includes}
           -D SPIRV
           -D TARGET_VULKAN
           --compiler "${SHADERMAKE_DXC_VK_PATH}"
           ${NVRHI_DEFAULT_VK_REGISTER_OFFSETS}
           --vulkanVersion ${VULKAN_VERSION}
           --shaderModel ${params_SHADER_MODEL}
           ${project_name_arg})

        list(APPEND compilerCommand ${params_SHADERMAKE_OPTIONS})
        list(APPEND compilerCommand ${params_SHADERMAKE_OPTIONS_SPIRV})

        if ("${params_BYPRODUCTS_SPIRV}" STREQUAL "")
            add_custom_command(TARGET ${params_TARGET} PRE_BUILD COMMAND ${compilerCommand})
        else()
            set(byproducts_with_paths "")
            foreach(relative_path IN LISTS params_BYPRODUCTS_SPIRV)
                list(APPEND byproducts_with_paths "${params_SPIRV_DXC}/${relative_path}")
            endforeach()

            add_custom_command(TARGET ${params_TARGET} PRE_BUILD COMMAND ${compilerCommand} BYPRODUCTS "${byproducts_with_paths}")
        endif()
    endif()

    if (params_SPIRV_SLANG AND DONUT_WITH_VULKAN)
        if (NOT EXISTS "${SHADERMAKE_SLANG_PATH}")
            message(FATAL_ERROR "donut_compile_shaders: Slang compiler not found --- please set SHADERMAKE_SLANG_PATH to the full path to the Slang executable")
        endif()
        
        set(compilerCommand ${SHADERMAKE_PATH}
           --config ${params_CONFIG}
           --out ${params_SPIRV_SLANG}
           --platform SPIRV
           ${output_format_arg}
           ${include_dirs}
           ${ignore_includes}
           -D SPIRV
           -D TARGET_VULKAN
           --compiler "${SHADERMAKE_SLANG_PATH}"
           --slang
           ${NVRHI_DEFAULT_VK_REGISTER_OFFSETS}
           --vulkanVersion ${VULKAN_VERSION}
           --shaderModel ${params_SHADER_MODEL}
           ${project_name_arg})

        list(APPEND compilerCommand ${params_SHADERMAKE_OPTIONS})
        list(APPEND compilerCommand ${params_SHADERMAKE_OPTIONS_SPIRV})

        if ("${params_BYPRODUCTS_SPIRV}" STREQUAL "")
            add_custom_command(TARGET ${params_TARGET} PRE_BUILD COMMAND ${compilerCommand})
        else()
            set(byproducts_with_paths "")
            foreach(relative_path IN LISTS params_BYPRODUCTS_SPIRV)
                list(APPEND byproducts_with_paths "${params_SPIRV_SLANG}/${relative_path}")
            endforeach()

            add_custom_command(TARGET ${params_TARGET} PRE_BUILD COMMAND ${compilerCommand} BYPRODUCTS "${byproducts_with_paths}")
        endif()
    endif()

    if(params_FOLDER)
        set_target_properties(${params_TARGET} PROPERTIES FOLDER ${params_FOLDER})
    endif()
    
    if(SHADERMAKE_TOOL)
        add_dependencies(${params_TARGET} ShaderMake)
    endif()

endfunction()

# Generates a build target that will compile shaders for a given config file for all enabled Donut platforms.
#
# When OUTPUT_FORMAT is BINARY or is unspecified, the shaders will be placed into subdirectories of
# ${OUTPUT_BASE}, with names compatible with the FindDirectoryWithShaderBin framework function.
# When OUTPUT_FORMAT is HEADER, the shaders for all platforms will be placed into OUTPUT_BASE directly,
# with platform-specific extensions: .dxbc.h, .dxil.h, .spirv.h.
#
# The BYPRODUCTS_NO_EXT argument lists all generated files without extensions and without base paths.
# Similar to donut_compile_shaders, the list of byproducts is needed to get correct incremental builds
# when using static (.h) shaders with Ninja build system.
#
# Usage:
#
# donut_compile_shaders_all_platforms(TARGET <generated build target name>
#                                     CONFIG <shader-config-file>
#                                     SOURCES <list>
#                                     OUTPUT_BASE <path>
#                                     [SLANG]
#                                     [FOLDER <folder-in-visual-studio-solution>]
#                                     [OUTPUT_FORMAT (HEADER|BINARY)]
#                                     [SHADERMAKE_OPTIONS <string>]       -- arguments passed to ShaderMake
#                                     [SHADERMAKE_OPTIONS_DXBC <string>]  -- same, only DXBC specific
#                                     [SHADERMAKE_OPTIONS_DXIL <string>]  -- same, only DXIL specific
#                                     [SHADERMAKE_OPTIONS_SPIRV <string>] -- same, only SPIR-V specific
#                                     [SHADER_MODEL <string>]             -- shader model n_n format (default: 6_5)
#                                     [BYPRODUCTS_NO_EXT <list>])         -- see the comment above
#                                     [INCLUDES <list>]                   -- include paths
#                                     [IGNORE_INCLUDES <list>])           -- list of included files for ShaderMake to ignore (e.g. c++)

function(donut_compile_shaders_all_platforms)
    set(options
        SLANG)
    set(oneValueArgs
        SHADERMAKE_OPTIONS
        SHADERMAKE_OPTIONS_DXBC
        SHADERMAKE_OPTIONS_DXIL
        SHADERMAKE_OPTIONS_SPIRV
        SHADER_MODEL
        COMPILER_OPTIONS        # deprecated
        COMPILER_OPTIONS_DXBC   # deprecated
        COMPILER_OPTIONS_DXIL   # deprecated
        COMPILER_OPTIONS_SPIRV  # deprecated
        CONFIG
        FOLDER
        OUTPUT_BASE
        OUTPUT_FORMAT
        PROJECT_NAME
        TARGET)
    set(multiValueArgs
        BYPRODUCTS_NO_EXT
        SOURCES
        INCLUDES
        IGNORE_INCLUDES
        RELAXED_INCLUDES)       # deprecated
    cmake_parse_arguments(params "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT params_TARGET)
        message(FATAL_ERROR "donut_compile_shaders_all_platforms: TARGET argument missing")
    endif()
    if (NOT params_CONFIG)
        message(FATAL_ERROR "donut_compile_shaders_all_platforms: CONFIG argument missing")
    endif()
    if (NOT params_OUTPUT_BASE)
        message(FATAL_ERROR "donut_compile_shaders_all_platforms: OUTPUT_BASE argument missing")
    endif()
    if (params_COMPILER_OPTIONS OR params_COMPILER_OPTIONS_DXBC OR
        params_COMPILER_OPTIONS_DXIL OR params_COMPILER_OPTIONS_SPIRV)
        message(SEND_ERROR "donut_compile_shaders_all_platforms: The COMPILER_OPTIONS[_platform] arguments "
                           "are deprecated, use SHADERMAKE_OPTIONS[_platform] instead")
    endif()
    if (params_RELAXED_INCLUDES)
        message(SEND_ERROR "donut_compile_shaders_all_platforms: The RELAXED_INCLUDES argument "
                           "is deprecated, use IGNORE_INCLUDES instead")
    endif()

    if ("${params_OUTPUT_FORMAT}" STREQUAL "HEADER")
        # Header/static compilation puts everything into one location and differentiates between platforms
        # using the .dxbc.h, .dxil.h, or .spirv.h extensions native to ${SHADERMAKE_PATH}
        set(output_dxbc ${params_OUTPUT_BASE})
        set(output_dxil ${params_OUTPUT_BASE})
        set(output_spirv ${params_OUTPUT_BASE})
    else()
        # Binary compilation puts shaders into per-platform folders - legacy mode compatible with various apps
        set(output_dxbc ${params_OUTPUT_BASE}/dxbc)
        set(output_dxil ${params_OUTPUT_BASE}/dxil)
        set(output_spirv ${params_OUTPUT_BASE}/spirv)
    endif()

    set(byproducts_dxbc "")
    set(byproducts_dxil "")
    set(byproducts_spirv "")
    foreach(byproduct IN LISTS params_BYPRODUCTS_NO_EXT)
        list(APPEND byproducts_dxbc "${byproduct}.dxbc.h")
        list(APPEND byproducts_dxil "${byproduct}.dxil.h")
        list(APPEND byproducts_spirv "${byproduct}.spirv.h")
    endforeach()
    
    if (params_SLANG)
        donut_compile_shaders(TARGET ${params_TARGET}
            CONFIG ${params_CONFIG}
            FOLDER ${params_FOLDER}
            DXIL_SLANG ${output_dxil}
            SPIRV_SLANG ${output_spirv}
            OUTPUT_FORMAT ${params_OUTPUT_FORMAT}
            PROJECT_NAME ${params_PROJECT_NAME}
            SHADERMAKE_OPTIONS ${params_SHADERMAKE_OPTIONS}
            SHADERMAKE_OPTIONS_DXIL ${params_SHADERMAKE_OPTIONS_DXIL}
            SHADERMAKE_OPTIONS_SPIRV ${params_SHADERMAKE_OPTIONS_SPIRV}
            SHADER_MODEL ${params_SHADER_MODEL}
            SOURCES ${params_SOURCES}
            INCLUDES ${params_INCLUDES}
            IGNORE_INCLUDES ${params_IGNORE_INCLUDES}
            BYPRODUCTS_DXIL ${byproducts_dxil}
            BYPRODUCTS_SPIRV ${byproducts_spirv})
    else()
        donut_compile_shaders(TARGET ${params_TARGET}
            CONFIG ${params_CONFIG}
            FOLDER ${params_FOLDER}
            DXBC ${output_dxbc}
            DXIL ${output_dxil}
            SPIRV_DXC ${output_spirv}
            OUTPUT_FORMAT ${params_OUTPUT_FORMAT}
            PROJECT_NAME ${params_PROJECT_NAME}
            SHADERMAKE_OPTIONS ${params_SHADERMAKE_OPTIONS}
            SHADERMAKE_OPTIONS_DXIL ${params_SHADERMAKE_OPTIONS_DXIL}
            SHADERMAKE_OPTIONS_DXBC ${params_SHADERMAKE_OPTIONS_DXBC}
            SHADERMAKE_OPTIONS_SPIRV ${params_SHADERMAKE_OPTIONS_SPIRV}
            SHADER_MODEL ${params_SHADER_MODEL}
            SOURCES ${params_SOURCES}
            INCLUDES ${params_INCLUDES}
            IGNORE_INCLUDES ${params_IGNORE_INCLUDES}
            BYPRODUCTS_DXBC ${byproducts_dxbc}
            BYPRODUCTS_DXIL ${byproducts_dxil}
            BYPRODUCTS_SPIRV ${byproducts_spirv})
    endif()

endfunction()
