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

// Selects the NVRHI cluster-operation layouts for the target being compiled. DXIL for an NVRHI
// built with NVRHI_D3D12_WITH_DXRCLUSTER takes the DXR clusters layouts; NVAPI and Vulkan share the nv ones.
// donut_compile_shaders passes NVRHI_D3D12_WITH_DXRCLUSTER to DXIL compiles when it is enabled.
#if defined(TARGET_D3D12) && NVRHI_D3D12_WITH_DXRCLUSTER

#define DONUT_CLUSTER_ARGS_DXR 1

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

#define DONUT_CLUSTER_ARGS_DXR 0

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
