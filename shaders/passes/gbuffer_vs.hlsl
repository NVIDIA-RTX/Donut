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

#include <donut/shaders/bindless.h>
#include <donut/shaders/forward_vertex.hlsli>
#include <donut/shaders/gbuffer_cb.h>
#include <donut/shaders/binding_helpers.hlsli>
#include <donut/shaders/packing.hlsli>

DECLARE_CBUFFER(GBufferFillConstants, c_GBuffer, GBUFFER_BINDING_VIEW_CONSTANTS, GBUFFER_SPACE_VIEW);

DECLARE_PUSH_CONSTANTS(GBufferPushConstants, g_Push, GBUFFER_BINDING_PUSH_CONSTANTS, GBUFFER_SPACE_INPUT);

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
#if MOTION_VECTORS
    in float4 i_prevInstanceMatrix0 : PREV_TRANSFORM0,
    in float4 i_prevInstanceMatrix1 : PREV_TRANSFORM1,
    in float4 i_prevInstanceMatrix2 : PREV_TRANSFORM2,
#endif
    in uint i_instance : SV_InstanceID,
    out float4 o_position : SV_Position,
    out SceneVertex o_vtx,
    out uint o_instance : INSTANCE
)
{
    float3x4 instanceMatrix = float3x4(i_instanceMatrix0, i_instanceMatrix1, i_instanceMatrix2);

    o_vtx = i_vtx;
    o_vtx.pos = mul(instanceMatrix, float4(i_vtx.pos, 1.0)).xyz;
    o_vtx.normal = mul(instanceMatrix, float4(i_vtx.normal, 0)).xyz;
    o_vtx.tangent.xyz = mul(instanceMatrix, float4(i_vtx.tangent.xyz, 0)).xyz;
    o_vtx.tangent.w = i_vtx.tangent.w;
#if MOTION_VECTORS
    float3x4 prevInstanceMatrix = float3x4(i_prevInstanceMatrix0, i_prevInstanceMatrix1, i_prevInstanceMatrix2);
    o_vtx.prevPos = mul(prevInstanceMatrix, float4(i_vtx.prevPos, 1.0)).xyz;
#else
    o_vtx.prevPos = o_vtx.pos;
#endif

    float4 worldPos = float4(o_vtx.pos, 1.0);
    float4 viewPos = mul(worldPos, c_GBuffer.view.matWorldToView);
    o_position = mul(viewPos, c_GBuffer.view.matViewToClip);
    o_instance = i_instance;

    o_vtx.texCoord = DecodeTexCoord<DECODE_TEXCOORD != 0>(o_vtx.texCoord, g_Push.texCoordFormat, g_Push.texCoordScaleBias);
}

// Use a raw buffer on DX11 to avoid adding the StructuredBuffer flag to the instance buffer.
// On DX11, a structured buffer cannot be used as a vertex buffer, and there should be compatibility with other passes.
// On DX12, using a structured buffer results in more optimal code being generated.
#ifdef TARGET_D3D11
ByteAddressBuffer t_Instances : REGISTER_SRV(GBUFFER_BINDING_INSTANCE_BUFFER, GBUFFER_SPACE_INPUT);
#else
StructuredBuffer<InstanceData> t_Instances : REGISTER_SRV(GBUFFER_BINDING_INSTANCE_BUFFER, GBUFFER_SPACE_INPUT);
#endif
ByteAddressBuffer t_Vertices : REGISTER_SRV(GBUFFER_BINDING_VERTEX_BUFFER, GBUFFER_SPACE_INPUT);


// Version of the vertex shader that uses buffer loads to read vertex attributes and transforms.
void buffer_loads(
    in uint i_vertex : SV_VertexID,
	in uint i_instance : SV_InstanceID,
    out float4 o_position : SV_Position,
    out SceneVertex o_vtx,
    out uint o_instance : INSTANCE
)
{
    o_instance = i_instance;

    i_instance += g_Push.startInstanceLocation;
    i_vertex += g_Push.startVertexLocation;

#ifdef TARGET_D3D11
    const InstanceData instance = LoadInstanceData(t_Instances, i_instance * c_SizeOfInstanceData);
#else
    const InstanceData instance = t_Instances[i_instance];
#endif

    float3 pos = asfloat(t_Vertices.Load3(g_Push.positionOffset + i_vertex * c_SizeOfPosition));
    float3 prevPos = asfloat(t_Vertices.Load3(g_Push.prevPositionOffset + i_vertex * c_SizeOfPosition));
    float2 texCoord = LoadTexCoord(t_Vertices, g_Push.texCoordOffset, i_vertex, g_Push.texCoordFormat, g_Push.texCoordScaleBias);
    uint packedNormal = t_Vertices.Load(g_Push.normalOffset + i_vertex * c_SizeOfNormal);
    uint packedTangent = t_Vertices.Load(g_Push.tangentOffset + i_vertex * c_SizeOfNormal);
    float3 normal = Unpack_RGB8_SNORM(packedNormal);
    float4 tangent = Unpack_RGBA8_SNORM(packedTangent);

    o_vtx.pos = mul(instance.transform, float4(pos, 1.0)).xyz;
    o_vtx.texCoord = texCoord;
    o_vtx.normal = mul(instance.transform, float4(normal, 0)).xyz;
    o_vtx.tangent.xyz = mul(instance.transform, float4(tangent.xyz, 0)).xyz;
    o_vtx.tangent.w = tangent.w;
#if MOTION_VECTORS
    o_vtx.prevPos = mul(instance.prevTransform, float4(prevPos, 1.0)).xyz;
#else
    o_vtx.prevPos = o_vtx.pos;
#endif

    float4 worldPos = float4(o_vtx.pos, 1.0);
    o_position = mul(worldPos, c_GBuffer.view.matWorldToClip);

}
