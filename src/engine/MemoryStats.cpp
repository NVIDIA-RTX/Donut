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

#include <donut/engine/MemoryStats.h>
#include <donut/engine/Scene.h>
#include <donut/engine/SceneTypes.h>

#include <unordered_set>

namespace donut::engine
{
namespace
{
    void addBuffer(nvrhi::IDevice* device, nvrhi::IBuffer* buffer, GpuMemoryUsage& stats,
        std::unordered_set<nvrhi::IBuffer*>& visited)
    {
        if (buffer == nullptr || !visited.insert(buffer).second)
            return;

        stats.resourceCount++;
        stats.capacityBytes += buffer->getDesc().byteSize;

        uint64_t allocationBytes = 0;
        if (tryGetResourceAllocationBytes(device, buffer, allocationBytes))
            stats.allocationBytes += allocationBytes;
        else
            stats.unknownAllocationCount++;
    }
}

bool tryGetResourceAllocationBytes(nvrhi::IDevice* device, nvrhi::IResource* resource, uint64_t& outBytes)
{
    if (device == nullptr || resource == nullptr)
        return false;

    // NVRHI asserts for these unsupported queries in debug builds.
    if (device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11 || dynamic_cast<nvrhi::ITexture*>(resource))
        return false;

    nvrhi::MemoryRequirements requirements;
    if (!resource->queryMemoryRequirements(requirements))
        return false;

    outBytes = requirements.size;
    return true;
}

SceneGeometryMemoryStats getSceneGeometryMemoryStats(nvrhi::IDevice* device, const Scene& scene)
{
    SceneGeometryMemoryStats stats;

    const std::shared_ptr<SceneGraph> sceneGraph = scene.GetSceneGraph();
    if (sceneGraph == nullptr)
        return stats;

    std::unordered_set<nvrhi::IBuffer*> vertexBuffers;
    std::unordered_set<nvrhi::IBuffer*> indexBuffers;

    for (const std::shared_ptr<MeshInfo>& mesh : sceneGraph->GetMeshes())
    {
        if (mesh == nullptr || mesh->buffers == nullptr)
            continue;

        addBuffer(device, mesh->buffers->vertexBuffer, stats.vertexBuffers, vertexBuffers);
        addBuffer(device, mesh->buffers->indexBuffer, stats.indexBuffers, indexBuffers);
    }

    return stats;
}
}
