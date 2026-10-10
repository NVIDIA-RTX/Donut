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

#ifndef GBUFFER_CB_H
#define GBUFFER_CB_H

#include "view_cb.h"

#define GBUFFER_SPACE_MATERIAL 0
#define GBUFFER_BINDING_MATERIAL_CONSTANTS 0
#define GBUFFER_BINDING_MATERIAL_DIFFUSE_TEXTURE 0
#define GBUFFER_BINDING_MATERIAL_SPECULAR_TEXTURE 1
#define GBUFFER_BINDING_MATERIAL_NORMAL_TEXTURE 2
#define GBUFFER_BINDING_MATERIAL_EMISSIVE_TEXTURE 3
#define GBUFFER_BINDING_MATERIAL_OCCLUSION_TEXTURE 4
#define GBUFFER_BINDING_MATERIAL_TRANSMISSION_TEXTURE 5
#define GBUFFER_BINDING_MATERIAL_OPACITY_TEXTURE 6

#define GBUFFER_SPACE_INPUT 1
#define GBUFFER_BINDING_PUSH_CONSTANTS 1
#define GBUFFER_BINDING_INSTANCE_BUFFER 10
#define GBUFFER_BINDING_VERTEX_BUFFER 11

#define GBUFFER_SPACE_VIEW 2
#define GBUFFER_BINDING_VIEW_CONSTANTS 2
#define GBUFFER_BINDING_MATERIAL_SAMPLER 0

struct GBufferFillConstants
{
    PlanarViewConstants view;
    PlanarViewConstants viewPrev;
};

struct GBufferPushConstants
{
    uint        startInstanceLocation;
    uint        startVertexLocation;
    uint        positionOffset;
    uint        prevPositionOffset;
    uint        texCoordOffset;
    uint        normalOffset;
    uint        tangentOffset;
    uint        texCoordFormat;
    float4      texCoordScaleBias;
};

#endif // GBUFFER_CB_H
