// Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma pack_matrix(row_major)

#include <donut/shaders/forward_vertex.hlsli>
#include <donut/shaders/depth_cb.h>
#include <donut/shaders/material_cb.h>
#include <donut/shaders/bindless.h>
#include <donut/shaders/binding_helpers.hlsli>

DECLARE_CBUFFER(DepthPassConstants, g_CustomDepth, DEPTH_BINDING_VIEW_CONSTANTS, DEPTH_SPACE_VIEW);
DECLARE_PUSH_CONSTANTS(DepthPushConstants, g_CustomPush, DEPTH_BINDING_PUSH_CONSTANTS, DEPTH_SPACE_INPUT);
// Depth, Forward and GBuffer all bind MaterialConstants at b0, space0.
DECLARE_CBUFFER(MaterialConstants, g_TestMaterial, DEPTH_BINDING_MATERIAL_CONSTANTS, DEPTH_SPACE_MATERIAL);

// Distinguish a custom legacy vertex shader from the stock floating-point shader,
// including an application-defined use of a formerly per-draw constant in IA mode.
void custom_depth(
    in float3 position : POSITION,
    in float2 texCoord : TEXCOORD,
    in float3x4 transform : TRANSFORM,
    in uint instance : SV_InstanceID,
    out float4 outPosition : SV_Position,
    out float2 outTexCoord : TEXCOORD)
{
    float4 worldPosition = float4(mul(transform, float4(position, 1.0)), 1.0);
    outPosition = mul(worldPosition, g_CustomDepth.matWorldToClip);
    outTexCoord = DecodeTexCoord(texCoord, g_CustomPush.texCoordFormat, g_CustomPush.texCoordScaleBias)
        + float2(0.125 + 0.0625 * g_CustomPush.startVertexLocation, 0.25);
}

// Keep the production VS signatures, including unused fields, so semantic registers match.
float4 depth(float4 position : SV_Position, float2 texCoord : TEXCOORD) : SV_Target0
{
    return float4(texCoord, g_TestMaterial.baseOrDiffuseColor.x, 1.0);
}

float4 scene(float4 position : SV_Position, SceneVertex vertex) : SV_Target0
{
    return float4(vertex.texCoord, g_TestMaterial.baseOrDiffuseColor.x, 1.0);
}
