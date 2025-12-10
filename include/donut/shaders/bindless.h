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

#ifndef BINDLESS_H_
#define BINDLESS_H_

#include "material_cb.h"

struct GeometryData
{
    uint numIndices;
    uint numVertices;
    int indexBufferIndex;
    uint indexOffset;

    int vertexBufferIndex;
    uint positionOffset;
    uint prevPositionOffset;
    uint texCoord1Offset;

    uint texCoord2Offset;
    uint normalOffset;
    uint tangentOffset;
    uint curveRadiusOffset;

    uint materialIndex;
    uint pad0;
    uint pad1;
    uint pad2;
};

static const uint InstanceFlags_CurveDisjointOrthogonalTriangleStrips = 0x00000001u;
static const uint InstanceFlags_CurveLinearSweptSpheres = 0x00000002u;

struct InstanceData
{
    uint flags;
    uint firstGeometryInstanceIndex; // index into global list of geometry instances. 
                                     // foreach (Instance)
                                     //     foreach(Geo) index++
    uint firstGeometryIndex;         // index into global list of geometries. 
                                     // foreach(Mesh)
                                     //     foreach(Geo) index++
    uint numGeometries;

    float3x4 transform;
    float3x4 prevTransform;

    bool IsCurveDOTS() { return (flags & InstanceFlags_CurveDisjointOrthogonalTriangleStrips) != 0; }
    bool IsCurveLSS() { return (flags & InstanceFlags_CurveLinearSweptSpheres) != 0; }
};

#ifndef __cplusplus

static const uint c_SizeOfTriangleIndices = 12;
static const uint c_SizeOfPosition = 12;
static const uint c_SizeOfTexcoord = 8;
static const uint c_SizeOfNormal = 4;
static const uint c_SizeOfJointIndices = 8;
static const uint c_SizeOfJointWeights = 16;
static const uint c_SizeOfCurveRadius = 4;

// Define the sizes of these structures because FXC doesn't support sizeof(x)
static const uint c_SizeOfGeometryData = 4*16;
static const uint c_SizeOfInstanceData = 7*16;
static const uint c_SizeOfMaterialConstants = 13*16;

GeometryData LoadGeometryData(ByteAddressBuffer buffer, uint offset)
{
    uint4 a = buffer.Load4(offset + 16 * 0);
    uint4 b = buffer.Load4(offset + 16 * 1);
    uint4 c = buffer.Load4(offset + 16 * 2);
    uint4 d = buffer.Load4(offset + 16 * 3);

    GeometryData ret;
    ret.numIndices = a.x;
    ret.numVertices = a.y;
    ret.indexBufferIndex = int(a.z);
    ret.indexOffset = a.w;
    ret.vertexBufferIndex = int(b.x);
    ret.positionOffset = b.y;
    ret.prevPositionOffset = b.z;
    ret.texCoord1Offset = b.w;
    ret.texCoord2Offset = c.x;
    ret.normalOffset = c.y;
    ret.tangentOffset = c.z;
    ret.curveRadiusOffset = c.w;
    ret.materialIndex = d.x;
    ret.pad0 = d.y;
    ret.pad1 = d.z;
    ret.pad2 = d.w;
    return ret;
}

InstanceData LoadInstanceData(ByteAddressBuffer buffer, uint offset)
{
    uint4 a = buffer.Load4(offset + 16 * 0);
    uint4 b = buffer.Load4(offset + 16 * 1);
    uint4 c = buffer.Load4(offset + 16 * 2);
    uint4 d = buffer.Load4(offset + 16 * 3);
    uint4 e = buffer.Load4(offset + 16 * 4);
    uint4 f = buffer.Load4(offset + 16 * 5);
    uint4 g = buffer.Load4(offset + 16 * 6);

    InstanceData ret;
    ret.flags = a.x;
    ret.firstGeometryInstanceIndex = a.y;
    ret.firstGeometryIndex = a.z;
    ret.numGeometries = a.w;
    ret.transform = float3x4(asfloat(b), asfloat(c), asfloat(d));
    ret.prevTransform = float3x4(asfloat(e), asfloat(f), asfloat(g));
    return ret;
}

MaterialConstants LoadMaterialConstants(ByteAddressBuffer buffer, uint offset)
{
    uint4 a = buffer.Load4(offset + 16 * 0);
    uint4 b = buffer.Load4(offset + 16 * 1);
    uint4 c = buffer.Load4(offset + 16 * 2);
    uint4 d = buffer.Load4(offset + 16 * 3);
    uint4 e = buffer.Load4(offset + 16 * 4);
    uint4 f = buffer.Load4(offset + 16 * 5);
    uint4 g = buffer.Load4(offset + 16 * 6);
    uint4 h = buffer.Load4(offset + 16 * 7);
    uint4 i = buffer.Load4(offset + 16 * 8);
    uint4 j = buffer.Load4(offset + 16 * 9);
    uint4 k = buffer.Load4(offset + 16 * 10);
    uint4 l = buffer.Load4(offset + 16 * 11);
    uint4 m = buffer.Load4(offset + 16 * 12);

    MaterialConstants ret;
    ret.baseOrDiffuseColor = asfloat(a.xyz);
    ret.flags = int(a.w);
    ret.specularColor = asfloat(b.xyz);
    ret.materialID = int(b.w);
    ret.emissiveColor = asfloat(c.xyz);
    ret.domain = int(c.w);
    ret.opacity = asfloat(d.x);
    ret.roughness = asfloat(d.y);
    ret.metalness = asfloat(d.z);
    ret.normalTextureScale = asfloat(d.w);
    ret.occlusionStrength = asfloat(e.x);
    ret.alphaCutoff = asfloat(e.y);
    ret.transmissionFactor = asfloat(e.z);
    ret.baseOrDiffuseTextureIndex = int(e.w);
    ret.metalRoughOrSpecularTextureIndex = int(f.x);
    ret.emissiveTextureIndex = int(f.y);
    ret.normalTextureIndex = int(f.z);
    ret.occlusionTextureIndex = int(f.w);
    ret.transmissionTextureIndex = int(g.x);
    ret.opacityTextureIndex = int(g.y);
    ret.normalTextureTransformScale = asfloat(g.zw);
    ret.padding1 = h.xyz;
    ret.sssScale = int(h.w);
    ret.sssTransmissionColor = asfloat(i.xyz);
    ret.sssAnisotropy = asfloat(i.w);
    ret.sssScatteringColor = asfloat(j.xyz);
    ret.hairMelanin = asfloat(j.w);
    ret.hairBaseColor = asfloat(k.xyz);
    ret.hairMelaninRedness = asfloat(k.w);
    ret.hairLongitudinalRoughness = asfloat(l.x);
    ret.hairAzimuthalRoughness = asfloat(l.y);
    ret.hairIor = asfloat(l.z);
    ret.hairCuticleAngle = asfloat(l.w);
    ret.hairDiffuseReflectionTint = asfloat(m.xyz);
    ret.hairDiffuseReflectionWeight = asfloat(m.w);
    return ret;   
}

#endif

#endif // BINDLESS_H_