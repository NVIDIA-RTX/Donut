#
# Copyright (c) 2021-2025, NVIDIA CORPORATION. All rights reserved.
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

set(dlss_sdk "${dlss_SOURCE_DIR}")
set(dlss_platform_lin "Linux_x86_64")

# The SDK ships per-architecture Windows binaries (310.9+ for Arm64). The x64 import libraries
# live in an extra "x64" subdirectory, while the Arm64 ones sit directly in the platform directory.
if (NVRHI_TARGET_ARCH STREQUAL "arm64")
	set(dlss_platform_win "Windows_aarch64")
	set(dlss_implib_dir_win "${dlss_sdk}/lib/${dlss_platform_win}")
else()
	set(dlss_platform_win "Windows_x86_64")
	set(dlss_implib_dir_win "${dlss_sdk}/lib/${dlss_platform_win}/x64")
endif()

set(dlss_lib_release "nvsdk_ngx_s.lib")
set(dlss_lib_debug "nvsdk_ngx_s_dbg.lib")
	
if (WIN32)
	add_library(DLSS SHARED IMPORTED)

	set_target_properties(DLSS PROPERTIES
		IMPORTED_IMPLIB "${dlss_implib_dir_win}/${dlss_lib_release}"
		IMPORTED_IMPLIB_DEBUG "${dlss_implib_dir_win}/${dlss_lib_debug}"
		IMPORTED_LOCATION "${dlss_sdk}/lib/${dlss_platform_win}/rel/nvngx_dlss.dll"
		IMPORTED_LOCATION_DEBUG "${dlss_sdk}/lib/${dlss_platform_win}/dev/nvngx_dlss.dll"
	)

	set(DLSS_SHARED_LIBRARY_PATH "${dlss_sdk}/lib/${dlss_platform_win}/$<IF:$<CONFIG:Debug>,dev,rel>/nvngx_dlss.dll" PARENT_SCOPE)

elseif (UNIX AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
	add_library(DLSS STATIC IMPORTED)


	# Search for the DLSS library files, because the version number is included in the filename,
	# and we don't know which version was pulled - that's controlled by DONUT_DLSS_FETCH_TAG.

	set(dlss_lib_search_path "${dlss_sdk}/lib/${dlss_platform_lin}/rel/libnvidia-ngx-dlss.so.*")
	file(GLOB dlss_lib_files ${dlss_lib_search_path})

	if (dlss_lib_files)
		list(LENGTH dlss_lib_files dlss_lib_files_count)
		if (dlss_lib_files_count EQUAL 1)
			get_filename_component(dlss_lib "${dlss_lib_files}" NAME)
			message(STATUS "Found DLSS shared library file: ${dlss_lib}")
		else()
			message(FATAL_ERROR "Expected exactly one DLSS library file, found ${dlss_lib_files_count}")
		endif()
	else()
		message(FATAL_ERROR "No DLSS library files found at ${dlss_lib_search_path}")
	endif()
	
	set_target_properties(DLSS PROPERTIES
		IMPORTED_LOCATION "${dlss_sdk}/lib/${dlss_platform_lin}/libnvsdk_ngx.a"
	)

	set(DLSS_SHARED_LIBRARY_PATH "${dlss_sdk}/lib/${dlss_platform_lin}/$<IF:$<CONFIG:Debug>,dev,rel>/${dlss_lib}" PARENT_SCOPE)

else()
	message("DLSS is not supported on the target platform.")
endif()

if (TARGET DLSS)
	target_include_directories(DLSS INTERFACE "${dlss_sdk}/include")
endif()

# ---- DLSS RR (Ray Reconstruction) ----

if (WIN32)
	add_library(DLSS_RR SHARED IMPORTED)

	set_target_properties(DLSS_RR PROPERTIES
		IMPORTED_IMPLIB "${dlss_implib_dir_win}/${dlss_lib_release}"
		IMPORTED_IMPLIB_DEBUG "${dlss_implib_dir_win}/${dlss_lib_debug}"
		IMPORTED_LOCATION "${dlss_sdk}/lib/${dlss_platform_win}/rel/nvngx_dlssd.dll"
		IMPORTED_LOCATION_DEBUG "${dlss_sdk}/lib/${dlss_platform_win}/dev/nvngx_dlssd.dll"
	)

	set(DLSS_RR_SHARED_LIBRARY_PATH "${dlss_sdk}/lib/${dlss_platform_win}/$<IF:$<CONFIG:Debug>,dev,rel>/nvngx_dlssd.dll" PARENT_SCOPE)

elseif (UNIX AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
	add_library(DLSS_RR STATIC IMPORTED)

	# Search for the DLSS_RR library files, because the version number is included in the filename,
	# and we don't know which version was pulled - that's controlled by DONUT_DLSS_FETCH_TAG.

	set(dlss_lib_search_path "${dlss_sdk}/lib/${dlss_platform_lin}/rel/libnvidia-ngx-dlssd.so.*")
	file(GLOB dlss_lib_files ${dlss_lib_search_path})

	if (dlss_lib_files)
		list(LENGTH dlss_lib_files dlss_lib_files_count)
		if (dlss_lib_files_count EQUAL 1)
			get_filename_component(dlss_lib "${dlss_lib_files}" NAME)
			message(STATUS "Found DLSS RR shared library file: ${dlss_lib}")
		else()
			message(FATAL_ERROR "Expected exactly one DLSS RR library file, found ${dlss_lib_files_count}")
		endif()
	else()
		message(FATAL_ERROR "No DLSS RR library files found at ${dlss_lib_search_path}")
	endif()
	
	set_target_properties(DLSS_RR PROPERTIES
		IMPORTED_LOCATION "${dlss_sdk}/lib/${dlss_platform_lin}/libnvsdk_ngx.a"
	)

	set(DLSS_RR_SHARED_LIBRARY_PATH "${dlss_sdk}/lib/${dlss_platform_lin}/$<IF:$<CONFIG:Debug>,dev,rel>/${dlss_lib}" PARENT_SCOPE)

else()
	message("DLSS RR is not supported on the target platform.")
endif()

if (TARGET DLSS_RR)
	target_include_directories(DLSS_RR INTERFACE "${dlss_sdk}/include")
endif()
