#
# Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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


include(../compileshaders.cmake)

file(GLOB donut_shaders
	"*.hlsl"
	"passes/*.hlsl"
	"../include/donut/shaders/*.hlsli"
	"../include/donut/shaders/*.h"
)

if(DONUT_SHADERS_OUTPUT_DIR AND NOT DONUT_WITH_STATIC_SHADERS)
	set(output_base ${DONUT_SHADERS_OUTPUT_DIR})
else()
	set(output_base ${CMAKE_CURRENT_BINARY_DIR}/compiled_shaders)
endif()

if(DONUT_WITH_STATIC_SHADERS)
	set(output_format "HEADER")
else()
	set(output_format "BINARY")
endif()

# List all outputs of the shader compile target to ensure proper rebuilds when using Ninja
set(byproducts 
	blit_ps
	fullscreen_vs
	ies_profile_cs
	imgui_pixel
	imgui_vertex
	rect_vs
	sharpen_ps
	skinning_cs
	passes/bloom_ps
	passes/cubemap_gs
	passes/deferred_lighting_cs
	passes/depth_ps
	passes/depth_vs
	passes/dlss_exposure_cs
	passes/environment_map_ps
	passes/exposure_cs
	passes/forward_ps
	passes/forward_vs
	passes/gbuffer_ps
	passes/gbuffer_vs
	passes/manipulator_vs_main
	passes/manipulator_gs_main
	passes/manipulator_ps_main
	passes/manipulator_scan_cs
	passes/histogram_cs
	passes/joints_main_ps
	passes/joints_main_vs
	passes/light_probe_cubemap_gs
	passes/light_probe_diffuse_probe_ps
	passes/light_probe_environment_brdf_ps
	passes/light_probe_mip_ps
	passes/light_probe_specular_probe_ps
	passes/material_id_ps
	passes/mipmapgen_cs
	passes/motion_vectors_ps
	passes/pixel_readback_cs
	passes/sky_ps
	passes/ssao_blur_cs
	passes/ssao_compute_cs
	passes/ssao_deinterleave_cs
	passes/taa_cs
	passes/tonemapping_ps
)

if(DONUT_WITH_AFTERMATH OR DONUT_EMBED_SHADER_PDBS)
    set(shadermake_options --embedPDB)
endif()

donut_compile_shaders_all_platforms(
	TARGET donut_shaders
	PROJECT_NAME "Donut"
	CONFIG ${CMAKE_CURRENT_LIST_DIR}/DonutShaders.cfg
	FOLDER Donut
	OUTPUT_BASE ${output_base}
	OUTPUT_FORMAT ${output_format}
	SOURCES ${donut_shaders}
	BYPRODUCTS_NO_EXT ${byproducts}
    SHADERMAKE_OPTIONS ${shadermake_options}
)
