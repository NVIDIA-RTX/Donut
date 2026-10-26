/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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

#ifndef CLUSTER_ARGS_HLSLI
#define CLUSTER_ARGS_HLSLI

#include <nvrhi/nvrhiHLSL.h>

// Selects the NVRHI cluster-operation layouts for the shader being compiled. They follow the
// device's nvrhi::ClusterApi, so a D3D12 shader that uses them is compiled twice, and the
// variant for devices with DXR clusters defines DONUT_CLUSTER_API_DXR=1.
#ifndef DONUT_CLUSTER_API_DXR
#define DONUT_CLUSTER_API_DXR 0
#endif

#if DONUT_CLUSTER_API_DXR && !defined(TARGET_D3D12)
#error "DONUT_CLUSTER_API_DXR set for a non-D3D12 target: Vulkan always uses the nv layouts, so add the DXR cluster variant to DXIL only"
#endif

#if DONUT_CLUSTER_API_DXR
namespace donut
{
    namespace cluster
    {
        using IndirectTriangleClasArgs        = nvrhi::rt::cluster::dxr::IndirectTriangleClasArgs;
        using IndirectTriangleTemplateArgs    = nvrhi::rt::cluster::dxr::IndirectTriangleTemplateArgs;
        using IndirectInstantiateTemplateArgs = nvrhi::rt::cluster::dxr::IndirectInstantiateTemplateArgs;
        using OperationSize                   = nvrhi::rt::cluster::dxr::OperationSize;
    }
}
#else
namespace donut
{
    namespace cluster
    {
        using IndirectTriangleClasArgs        = nvrhi::rt::cluster::nv::IndirectTriangleClasArgs;
        using IndirectTriangleTemplateArgs    = nvrhi::rt::cluster::nv::IndirectTriangleTemplateArgs;
        using IndirectInstantiateTemplateArgs = nvrhi::rt::cluster::nv::IndirectInstantiateTemplateArgs;
        using OperationSize                   = nvrhi::rt::cluster::nv::OperationSize;
    }
}
#endif

#endif // CLUSTER_ARGS_HLSLI
