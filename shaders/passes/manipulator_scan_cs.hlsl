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

// Hover scan: pick the manipulator widget whose nearest rendered pixel is closest to the cursor.

#include <donut/shaders/manipulator_cb.h>

cbuffer c_Scan : register(b0) { ManipulatorScanConstants g_Scan; }

Texture2D<uint> t_Pick : register(t0);
RWBuffer<uint> u_Result : register(u0); // [0] = widget id, [1] = squared distance to widget

static const uint widget_none = 0;

// Pack the widget ID into least-significant bits so we can resolve the closest widget 
// with atomic min()
struct ScanKey
{
    static const uint kEmpty = 0xFFFFFFFFu;
    static const uint kWidgetBits = 24;    
    
    uint packed;

    void encode(uint distSq, uint widget) { packed = (distSq << kWidgetBits) | widget; }     

    bool empty() { return packed == kEmpty; }
    uint widget() { return packed & ((1u << kWidgetBits) - 1u); }
    uint distanceSq() { return packed >> kWidgetBits; }
};

groupshared ScanKey s_best;

[numthreads(MANIPULATOR_SCAN_TILE, MANIPULATOR_SCAN_TILE, 1)]
void main(uint2 localId : SV_GroupThreadID)
{
    if (all(localId == uint2(0, 0)))
        s_best.packed = ScanKey::kEmpty;

    GroupMemoryBarrierWithGroupSync();

    int2 size = min(g_Scan.scanSize, int2(MANIPULATOR_SCAN_TILE, MANIPULATOR_SCAN_TILE));

    if ((int(localId.x) < size.x) && (int(localId.y) < size.y))
    {
        int2 coord = g_Scan.pixel - size / 2 + int2(localId);
        if (all(coord >= int2(0, 0)) && all(coord < g_Scan.viewportSize))
        {
            uint widget = t_Pick.Load(int3(coord, 0));
            if (widget != widget_none)
            {
                int2 off = int2(localId) - size / 2;
                
                ScanKey key;
                key.encode(uint(dot(off, off)), widget);
                
                InterlockedMin(s_best.packed, key.packed);
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (all(localId == uint2(0, 0)))
    {
        u_Result[0] = !s_best.empty() ? s_best.widget() : widget_none;
        u_Result[1] = !s_best.empty() ? s_best.distanceSq() : 0;
    }
}
