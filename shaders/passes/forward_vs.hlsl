/*
* Copyright (c) 2014-2024, NVIDIA CORPORATION. All rights reserved.
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

#pragma pack_matrix(row_major)

#include <donut/shaders/forward_cb.h>
#include <donut/shaders/forward_vertex.hlsli>
#include <donut/shaders/binding_helpers.hlsli>
#include <donut/shaders/bindless.h>
#include <donut/shaders/packing.hlsli>

DECLARE_CBUFFER(ForwardShadingViewConstants, g_ForwardView, FORWARD_BINDING_VIEW_CONSTANTS, FORWARD_SPACE_VIEW);

// Version of the vertex shader that uses the hardware Input Assembler to read vertex attributes and transforms.
DECLARE_PUSH_CONSTANTS(ForwardPushConstants, g_Push, FORWARD_BINDING_PUSH_CONSTANTS, FORWARD_SPACE_INPUT);

// Selects the UV decoder template specialization for IA inputs.
// Keep the decoding default when compiling without an IA permutation define.
#ifndef DECODE_TEXCOORD
#define DECODE_TEXCOORD 1
#endif

void input_assembler(
	in SceneVertex i_vtx,
    in float4 i_instanceMatrix0 : TRANSFORM0,
    in float4 i_instanceMatrix1 : TRANSFORM1,
    in float4 i_instanceMatrix2 : TRANSFORM2,
    out float4 o_position : SV_Position,
    out SceneVertex o_vtx
)
{
    float3x4 instanceMatrix = float3x4(i_instanceMatrix0, i_instanceMatrix1, i_instanceMatrix2);

    o_vtx = i_vtx;
	o_vtx.pos = mul(instanceMatrix, float4(i_vtx.pos, 1.0)).xyz;
    o_vtx.normal = mul(instanceMatrix, float4(i_vtx.normal, 0)).xyz;
    o_vtx.tangent.xyz = mul(instanceMatrix, float4(i_vtx.tangent.xyz, 0)).xyz;
    o_vtx.tangent.w = i_vtx.tangent.w;

    float4 worldPos = float4(o_vtx.pos, 1.0);
    o_position = mul(worldPos, g_ForwardView.view.matWorldToClip);

    o_vtx.texCoord = DecodeTexCoord<DECODE_TEXCOORD != 0>(o_vtx.texCoord, g_Push.texCoordFormat, g_Push.texCoordScaleBias);
}


// Use a raw buffer on DX11 to avoid adding the StructuredBuffer flag to the instance buffer.
// On DX11, a structured buffer cannot be used as a vertex buffer, and there should be compatibility with other passes.
// On DX12, using a structured buffer results in more optimal code being generated.
#ifdef TARGET_D3D11
ByteAddressBuffer t_Instances               : REGISTER_SRV(FORWARD_BINDING_INSTANCE_BUFFER, FORWARD_SPACE_INPUT);
#else
StructuredBuffer<InstanceData> t_Instances  : REGISTER_SRV(FORWARD_BINDING_INSTANCE_BUFFER, FORWARD_SPACE_INPUT);
#endif
ByteAddressBuffer t_Vertices                : REGISTER_SRV(FORWARD_BINDING_VERTEX_BUFFER, FORWARD_SPACE_INPUT);


// Version of the vertex shader that uses buffer loads to read vertex attributes and transforms.
void buffer_loads(
    in uint i_vertex : SV_VertexID,
	in uint i_instance : SV_InstanceID,
    out float4 o_position : SV_Position,
    out SceneVertex o_vtx
)
{
    i_instance += g_Push.startInstanceLocation;
    i_vertex += g_Push.startVertexLocation;

#ifdef TARGET_D3D11
    const InstanceData instance = LoadInstanceData(t_Instances, i_instance * c_SizeOfInstanceData);
#else
    const InstanceData instance = t_Instances[i_instance];
#endif

    float3 pos = asfloat(t_Vertices.Load3(g_Push.positionOffset + i_vertex * c_SizeOfPosition));
    float2 texCoord = LoadTexCoord(t_Vertices, g_Push.texCoordOffset, i_vertex, g_Push.texCoordFormat, g_Push.texCoordScaleBias);
    uint packedNormal = t_Vertices.Load(g_Push.normalOffset + i_vertex * c_SizeOfNormal);
    uint packedTangent = t_Vertices.Load(g_Push.tangentOffset + i_vertex * c_SizeOfNormal);
    float3 normal = Unpack_RGB8_SNORM(packedNormal);
    float4 tangent = Unpack_RGBA8_SNORM(packedTangent);

    o_vtx.pos = mul(instance.transform, float4(pos, 1.0)).xyz;
    o_vtx.prevPos = o_vtx.pos;
    o_vtx.texCoord = texCoord;
    o_vtx.normal = mul(instance.transform, float4(normal, 0)).xyz;
    o_vtx.tangent.xyz = mul(instance.transform, float4(tangent.xyz, 0)).xyz;
    o_vtx.tangent.w = tangent.w;

    float4 worldPos = float4(o_vtx.pos, 1.0);
    o_position = mul(worldPos, g_ForwardView.view.matWorldToClip);
}
