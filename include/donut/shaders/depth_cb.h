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

#ifndef DEPTH_CB_H
#define DEPTH_CB_H

#define DEPTH_SPACE_MATERIAL 0
#define DEPTH_BINDING_MATERIAL_DIFFUSE_TEXTURE 0
#define DEPTH_BINDING_MATERIAL_OPACITY_TEXTURE 1
#define DEPTH_BINDING_MATERIAL_CONSTANTS 0

#define DEPTH_SPACE_INPUT 1
#define DEPTH_BINDING_PUSH_CONSTANTS 1
#define DEPTH_BINDING_INSTANCE_BUFFER 10
#define DEPTH_BINDING_VERTEX_BUFFER 11

#define DEPTH_SPACE_VIEW 2
#define DEPTH_BINDING_VIEW_CONSTANTS 2
#define DEPTH_BINDING_MATERIAL_SAMPLER 0

struct DepthPassConstants
{
    float4x4    matWorldToClip;
};

struct DepthPushConstants
{
    uint        startInstanceLocation;
    uint        startVertexLocation;
    uint        positionOffset;
    uint        texCoordOffset;
    uint        texCoordFormat;
    uint        padding0;
    uint        padding1;
    uint        padding2;
    float4      texCoordScaleBias;
};

#endif // DEPTH_CB_H
