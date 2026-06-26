/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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

#ifndef MANIPULATOR_CB_H
#define MANIPULATOR_CB_H

struct ManipulatorConstants
{
    float4 translate;       // world space
    float4 rotate;          // xyzw quaternion
    float4 scale;
    float4 rotationArc[2];  // arc endpoints (skip drawing if w == 0)

    uint selectedWidget;
    uint hoveredWidget;
    uint numSegments;       // tessellation of cones / rings / disks
    float manipScale;       // on-screen size, in pixel-ish units

    uint isDragging;
    uint3 pad;
};


#define MANIPULATOR_SCAN_TILE 16 // tile size for hover-scan compute pass

struct ManipulatorScanConstants
{
    int2 pixel;        // mouse cursor position (top-left origin)
    int2 scanSize;     // <= MANIPULATOR_SCAN_TILE
    int2 viewportSize;
    int2 pad;
};

#endif // MANIPULATOR_CB_H
