// Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
// SPDX-License-Identifier: MIT

#include <donut/shaders/bindless.h>
#include <donut/shaders/binding_helpers.hlsli>

struct TestConstants
{
    uint count;
    uint format;
    uint offset1;
    uint offset2;
};

DECLARE_PUSH_CONSTANTS(TestConstants, g_Test, 0, 0);
ByteAddressBuffer t_Vertices : register(t0);
ByteAddressBuffer t_Decodes : register(t1);
RWByteAddressBuffer u_Result : register(u0);

[numthreads(64, 1, 1)]
void main(uint vertex : SV_DispatchThreadID)
{
    if (vertex >= g_Test.count)
        return;

    for (uint stream = 0; stream < 2; ++stream)
    {
        uint baseOffset = stream == 0 ? g_Test.offset1 : g_Test.offset2;
        uint4 result = 0;
        if (baseOffset != ~0u)
        {
            float4 scaleBias = asfloat(t_Decodes.Load4(vertex * 32 + stream * 16));
            result.xy = asuint(LoadTexCoord(t_Vertices, baseOffset, vertex, g_Test.format, scaleBias));
            uint offset = baseOffset + vertex * GetTexCoordStride(g_Test.format);
            result.zw = g_Test.format == c_TexCoordFormat_Float32
                ? t_Vertices.Load2(offset) : uint2(t_Vertices.Load(offset), 0);
        }
        u_Result.Store4(vertex * 32 + stream * 16, result);
    }
}
