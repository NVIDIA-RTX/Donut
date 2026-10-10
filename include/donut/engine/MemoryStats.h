/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace donut::engine
{
    class Scene;

    struct GpuMemoryUsage
    {
        uint64_t capacityBytes = 0;
        uint64_t allocationBytes = 0;
        uint32_t resourceCount = 0;
        uint32_t unknownAllocationCount = 0;

        [[nodiscard]] bool hasKnownAllocationBytes() const { return unknownAllocationCount == 0; }
    };

    struct SceneGeometryMemoryStats
    {
        GpuMemoryUsage vertexBuffers;
        GpuMemoryUsage indexBuffers;
    };

    // Returns backing-buffer memory requirements, not residency or unique heap allocation bytes.
    // Pass the resource's owning device. Unsupported queries leave outBytes unchanged;
    // descriptor capacity remains separately available.
    bool tryGetResourceAllocationBytes(nvrhi::IDevice* device, nvrhi::IResource* resource, uint64_t& outBytes);

    // Deduplicates buffers within each category. Shared heaps and cross-category aliases need
    // application-specific attribution rather than summing these requirements as physical memory.
    SceneGeometryMemoryStats getSceneGeometryMemoryStats(nvrhi::IDevice* device, const Scene& scene);
}
