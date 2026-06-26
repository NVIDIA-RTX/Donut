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

// The manipulator is drawn procedurally: a point list is dispatched with no 
// vertex buffer and the geometry shader expands each point (by SV_PrimitiveID)
// into cones / cubes / rings / quads / lines.
//
// Variants (selected by ShaderMake value-defines, matched 1:1 at runtime):
//   MANIP_MODE       : 1 = translate, 2 = rotate, 3 = scale  (Manipulator::ManipMode)
//   ELEM_LINES       : 0 = solid triangles, 1 = outline lines
//   PICK_PASS        : 0 = color, 1 = write widget id (pixel shader only)
//   MANIPULATOR_STAGE: present only on the gs entry, guards the GS-only stream helpers so
//                      the vs/ps translation units (this file is shared) don't see them.

#pragma pack_matrix(row_major)

#include <donut/shaders/view_cb.h>
#include <donut/shaders/manipulator_cb.h>

cbuffer c_View : register(b0) { PlanarViewConstants g_View; }
cbuffer c_Manipulator : register(b1) { ManipulatorConstants g_Manip; }

// widget IDs (must match donut::render::Manipulator::Widget)
static const uint widget_none = 0;
static const uint widget_axis_x = 0x1;
static const uint widget_axis_y = 0x2;
static const uint widget_axis_z = 0x3;
static const uint widget_plane_xy = 0x4;
static const uint widget_plane_xz = 0x5;
static const uint widget_plane_yz = 0x6;
static const uint widget_plane_view = 0x7;
static const uint widget_undefined = 0x8;

static const float3 color_table[7] = {
    float3(1, 0, 0),                // red
    float3(0, 1, 0),                // green
    float3(0, 0, 1),                // blue
    float3(1, 1, 0),                // yellow (selected color)
    float3(0, 1, 1),                // cyan (alt selected)
    float3(1, 1, 1) * 0.3,          // grey
    float3(0.86, 0.71, 0.48) * 0.75 // tan (hovered)
};

static const uint widget_selected_color = 3;
static const uint widget_alt_selected_color = 4;
static const uint widget_none_color = 5;
static const uint widget_hovered_color = 6;

static const uint widget_colors[9] = { widget_none_color, 0, 1, 2, 2, 1, 0, 4, 5 };

bool widget_is_hovered(uint widget_id)
{
    return (g_Manip.hoveredWidget != widget_none) && (g_Manip.hoveredWidget == widget_id);
}
bool widget_is_interactive(uint widget_id)
{
    return (widget_id != widget_none) && (widget_id != widget_undefined);
}
uint get_widget_color_index(uint widget_id)
{
    uint color_index = (g_Manip.selectedWidget == widget_id) ? widget_selected_color : widget_colors[widget_id];
    if (!g_Manip.isDragging && widget_is_interactive(widget_id) && widget_is_hovered(widget_id))
        color_index = widget_hovered_color;
    return color_index;
}
bool is_axis_widget(uint widget)
{
    return (widget == widget_axis_x) || (widget == widget_axis_y) || (widget == widget_axis_z);
}
bool is_planar_widget(uint widget)
{
    return (widget == widget_plane_xy) || (widget == widget_plane_xz) || (widget == widget_plane_yz) || (widget == widget_plane_view);
}

struct PSIn
{
    float4 pos : SV_Position;
    nointerpolation uint widgetId : WIDGETID;
    nointerpolation float diffuse : DIFFUSE;
};

// Vertex Shader

// Emit a dummy clip position so the GS input signature (SV_Position) links.
float4 vs_main() : SV_Position { return float4(0.0, 0.0, 0.0, 1.0); }

#ifdef MANIPULATOR_STAGE

#if ELEM_LINES
typedef LineStream<PSIn> ManipulatorStream;
#else
typedef TriangleStream<PSIn> ManipulatorStream;
#endif

#if MANIP_MODE == 1
    #if ELEM_LINES
        #define MAXVERTS 26
    #else
        #define MAXVERTS 8
    #endif
#elif MANIP_MODE == 2
    #if ELEM_LINES
        #define MAXVERTS 19
    #else
        #define MAXVERTS 4
    #endif
#elif MANIP_MODE == 3
    #if ELEM_LINES
        #define MAXVERTS 26
    #else
        #define MAXVERTS 12
    #endif
#else
    #define MAXVERTS 1
#endif

static const float PI = 3.14159265358;

float3 apply_quat(float4 q, float3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

float compute_diffuse(float3 dx, float3 dy)
{
    float3 N = normalize(cross(dx, dy));
    N = mul(float4(N, 0.0), g_View.matWorldToClip).xyz;
    return clamp(dot(N, float3(0.0, 0.0, -1.0)), 0.0, 1.0);
}

float compute_screen_scale()
{
    float scale = 0.2 * mul(float4(g_Manip.translate.xyz, 1.0), g_View.matWorldToClip).w;
    scale *= g_Manip.manipScale * max(g_View.viewportSizeInv.x, g_View.viewportSizeInv.y);
    return scale;
}

bool is_widget_visible(uint widget)
{
    if (!g_Manip.isDragging)
        return true;
    uint selected = g_Manip.selectedWidget;
    if (is_axis_widget(selected))
        return selected == widget;
    if (is_planar_widget(selected))
    {
#if MANIP_MODE == 1 || MANIP_MODE == 3
        if (selected == widget_plane_xy)
            return (widget == widget_plane_xy) || (widget == widget_axis_x) || (widget == widget_axis_y);
        else if (selected == widget_plane_xz)
            return (widget == widget_plane_xz) || (widget == widget_axis_x) || (widget == widget_axis_z);
        else if (selected == widget_plane_yz)
            return (widget == widget_plane_yz) || (widget == widget_axis_y) || (widget == widget_axis_z);
        else if (selected == widget_plane_view)
            return (widget == widget_plane_view) || (widget == widget_axis_x) || (widget == widget_axis_y) || (widget == widget_axis_z);
#elif MANIP_MODE == 2
        return true;
#endif
    }
    return false;
}

// base primitives are generated in the XY plane, then rotated onto the widget's axis
void switch_axis(uint axis, inout float3 vert)
{
    // rotates a vertex in the xy plane to 
    if (axis == 1)
        vert = float3(vert.z, vert.x, vert.y);
    if (axis == 2)
        vert = float3(vert.y, vert.z, vert.x);
}
void switch_axis_arr(uint axis, inout float3 verts[4])
{
    for (int i = 0; i < 4; ++i)
        switch_axis(axis, verts[i]);
}
void switch_plane(uint axis, inout float3 vert)
{
    // rotates a vertex to be in the plane normal to the axis
    if (axis == 1)
        vert = float3(vert.y, vert.z, vert.x);
    if (axis == 2)
        vert = float3(vert.z, vert.x, vert.y);
}

float3 X_rotate(float angle) { return float3(0.0, cos(angle), sin(angle)); }
float3 Z_rotate(float angle) { return float3(cos(angle), sin(angle), 0.0); }

// helpers to generate cubes, squares, circles, cones, ...
void compute_circle_line_strip(uint axis, uint segment, float radius, inout float3 verts[5])
{
    uint num = g_Manip.numSegments;
    for (int i = 0; i < 5; ++i)
    {
        verts[i] = radius * X_rotate(2.0 * PI * float(segment * 4 + i) / float(num * 4 - 1));
        switch_axis(axis, verts[i]);
    }
}
void compute_square_tri_strip(float length, inout float3 verts[4])
{
    verts[0] = float3(-length, -length, 0.0); // "Z" pattern
    verts[1] = float3( length, -length, 0.0);
    verts[2] = float3(-length,  length, 0.0);
    verts[3] = float3( length,  length, 0.0);
}
void compute_square_line_strip(float length, inout float3 verts[5])
{
    verts[0] = float3(-length, -length, 0.0); // CCW pattern
    verts[1] = float3( length, -length, 0.0);
    verts[2] = float3( length,  length, 0.0);
    verts[3] = float3(-length,  length, 0.0);
    verts[4] = verts[0];
}
void compute_cone_tri_strip(uint axis, uint segment, inout float3 verts[4])
{
    const float base = 0.8;
    const float radius = 0.075;
    uint num = g_Manip.numSegments;
    float angle0 = 2.0 * PI * float(segment) / float(num - 1);
    float angle1 = 2.0 * PI * float(segment + 1) / float(num - 1);
    verts[0] = float3(1.0, 0.0, 0.0);
    verts[1] = float3(base, radius * X_rotate(angle0).yz);
    verts[2] = float3(base, radius * X_rotate(angle1).yz);
    verts[3] = float3(base, 0.0, 0.0);
    switch_axis_arr(axis, verts);
}
void compute_cube_tri_strip(uint face, inout float3 verts[4])
{
    const float length = 0.05;
    compute_square_tri_strip(length, verts);
    bool front = (face % 2) == 0;
    uint axis = face / 2;
    for (int i = 0; i < 4; ++i)
    {
        verts[i].z = front ? length : -length;
        switch_axis(axis, verts[i]);
    }
}
void compute_camembert_tri_strip(float3 v0, float3 v1, uint segment, float radius, inout float3 verts[4])
{
    uint num = g_Manip.numSegments;
    float angle = acos(clamp(dot(v0, v1), -1.0, 1.0));
    float a0 = angle * (float(segment) + 0.0) / float(num);
    float a1 = angle * (float(segment) + 0.5) / float(num);
    float a2 = angle * (float(segment) + 1.0) / float(num);
    float3 n = normalize(cross(v0, v1));
    verts[0] = (lerp(dot(n, v0) * n, v0, cos(a0)) + cross(n, v0) * sin(a0)) * radius;
    verts[1] = (lerp(dot(n, v0) * n, v0, cos(a1)) + cross(n, v0) * sin(a1)) * radius;
    verts[3] = (lerp(dot(n, v0) * n, v0, cos(a2)) + cross(n, v0) * sin(a2)) * radius;
    verts[2] = float3(0.0, 0.0, 0.0);
}

// emit helpers
void emit_vertex(inout ManipulatorStream stream, float3 p, uint widgetId, float diffuse)
{
    PSIn v;
    v.pos = mul(float4(p, 1.0), g_View.matWorldToClip);
    v.widgetId = widgetId;
    v.diffuse = diffuse;
    stream.Append(v);
}
void emit_vertex_clip(inout ManipulatorStream stream, float4 clip, uint widgetId, float diffuse)
{
    PSIn v;
    v.pos = clip;
    v.widgetId = widgetId;
    v.diffuse = diffuse;
    stream.Append(v);
}
void emit_1_line_strip(inout ManipulatorStream stream, float3 p0, float3 p1, uint widgetId, float diffuse)
{
    emit_vertex(stream, p0, widgetId, diffuse);
    emit_vertex(stream, p1, widgetId, diffuse);
    stream.RestartStrip();
}
void emit_4_line_strip(inout ManipulatorStream stream, float3 verts[5], uint widgetId, float diffuse)
{
    for (int i = 0; i < 5; ++i)
        emit_vertex(stream, verts[i], widgetId, diffuse);
    stream.RestartStrip();
}
void emit_2_tris_strip(inout ManipulatorStream stream, float3 verts[4], uint widgetId, bool no_light)
{
    float diffuse = no_light ? 0.0 : compute_diffuse(verts[1] - verts[0], verts[2] - verts[0]);
    emit_vertex(stream, verts[0], widgetId, diffuse);
    emit_vertex(stream, verts[1], widgetId, diffuse);
    emit_vertex(stream, verts[2], widgetId, diffuse);
    diffuse = no_light ? 0.0 : compute_diffuse(verts[3] - verts[1], verts[2] - verts[1]);
    emit_vertex(stream, verts[3], widgetId, diffuse);
    stream.RestartStrip();
}

void emit_line(inout ManipulatorStream stream, uint axis, float manip_scale, bool apply_transform, uint widgetId)
{
    float3 a = float3(0.0, 0.0, 0.0); a[axis] = 0.2;
    float3 b = float3(0.0, 0.0, 0.0); b[axis] = 1.0;
    if (apply_transform)
    {
        b *= g_Manip.scale.xyz;
        a = apply_quat(g_Manip.rotate, a);
        b = apply_quat(g_Manip.rotate, b);
    }
    emit_1_line_strip(stream, g_Manip.translate.xyz + manip_scale * a, g_Manip.translate.xyz + manip_scale * b, widgetId, 0.0);
}

float3 transform_planar_manip_vert(uint axis, float3 v, float3 offset)
{
    float3 s = g_Manip.scale.xyz;
    switch_axis(axis, s);
    v += (s * offset);
    switch_plane(axis, v);
    return apply_quat(g_Manip.rotate, v);
}
void emit_planar_square(inout ManipulatorStream stream, uint axis, float manip_scale, bool apply_transform, uint widgetId)
{
    const float length = 0.075;
    float3 verts[4];
    compute_square_tri_strip(length, verts);
    float3 offset = float3(0.5, 0.5, 0.0);
    for (int i = 0; i < 4; ++i)
    {
        if (apply_transform)
            verts[i] = transform_planar_manip_vert(axis, verts[i], offset);
        else
        {
            verts[i] += offset;
            switch_plane(axis, verts[i]);
        }
        verts[i] = g_Manip.translate.xyz + manip_scale * verts[i];
    }
    emit_2_tris_strip(stream, verts, widgetId, false);
}
void emit_planar_square_outline(inout ManipulatorStream stream, uint axis, float manip_scale, bool apply_transform, uint widgetId)
{
    const float length = 0.075;
    float3 verts[5];
    compute_square_line_strip(length, verts);
    float3 offset = float3(0.5, 0.5, 0.0);
    for (int i = 0; i < 5; ++i)
    {
        if (apply_transform)
            verts[i] = transform_planar_manip_vert(axis, verts[i], offset);
        else
        {
            verts[i] += offset;
            switch_plane(axis, verts[i]);
        }
        verts[i] = g_Manip.translate.xyz + manip_scale * verts[i];
    }
    emit_4_line_strip(stream, verts, widgetId, 0.0);
}
void emit_billboard_square_outline(inout ManipulatorStream stream, float manip_scale, uint widgetId)
{
    float length = manip_scale * 0.075;
    float3 verts[5];
    compute_square_line_strip(length, verts);
    float3 p = mul(float4(g_Manip.translate.xyz, 1.0), g_View.matWorldToView).xyz;
    for (int i = 0; i < 5; ++i)
        emit_vertex_clip(stream, mul(float4(p + verts[i], 1.0), g_View.matViewToClip), widgetId, 0.0);
    stream.RestartStrip();
}
void emit_cone_segment(inout ManipulatorStream stream, uint axis, uint segment, float manip_scale, uint widgetId)
{
    float3 verts[4];
    compute_cone_tri_strip(axis, segment, verts);
    for (int i = 0; i < 4; ++i)
        verts[i] = g_Manip.translate.xyz + manip_scale * verts[i];
    emit_2_tris_strip(stream, verts, widgetId, false);
}
void emit_cube_face(inout ManipulatorStream stream, uint face, float3 offset, float manip_scale, bool apply_rotation, uint widgetId)
{
    float3 verts[4];
    compute_cube_tri_strip(face, verts);
    for (int i = 0; i < 4; ++i)
    {
        float3 v = verts[i] + offset;
        if (apply_rotation)
            v = apply_quat(g_Manip.rotate, v);
        verts[i] = g_Manip.translate.xyz + manip_scale * v;
    }
    emit_2_tris_strip(stream, verts, widgetId, false);
}
void emit_camembert_segment(inout ManipulatorStream stream, uint segment, float radius, uint widgetId)
{
    float3 verts[4];
    compute_camembert_tri_strip(g_Manip.rotationArc[0].xyz, g_Manip.rotationArc[1].xyz, segment, radius, verts);
    for (int i = 0; i < 4; ++i)
        verts[i] = g_Manip.translate.xyz + verts[i];
    emit_2_tris_strip(stream, verts, widgetId, true);
}
void emit_circle_segment(inout ManipulatorStream stream, uint axis, uint segment, float radius, float scale, uint widgetId)
{
    float3 verts[5];
    compute_circle_line_strip(axis, segment, radius, verts);
    for (int i = 0; i < 5; ++i)
    {
        float3 vert = apply_quat(g_Manip.rotate, verts[i]);
        // Keep the camera-facing hemisphere. Donut view space is +Z-forward (opposite of COGL's
        // OpenGL -Z), so the camera-facing direction has negative view-z.
        if (dot(mul(float4(vert, 0.0), g_View.matWorldToView).xyz, float3(0.0, 0.0, -1.0)) < 0.0)
            break;
        emit_vertex(stream, g_Manip.translate.xyz + scale * vert, widgetId, 0.0);
    }
    stream.RestartStrip();
}
void emit_billboard_circle_segment(inout ManipulatorStream stream, uint segment, float radius, float manip_scale, uint widgetId)
{
    float3 verts[5];
    compute_circle_line_strip(2, segment, radius, verts);
    float3 p = mul(float4(g_Manip.translate.xyz, 1.0), g_View.matWorldToView).xyz;
    for (int i = 0; i < 5; ++i)
        emit_vertex_clip(stream, mul(float4(p + manip_scale * verts[i], 1.0), g_View.matViewToClip), widgetId, 0.0);
    stream.RestartStrip();
}

// Geometry Shader

[maxvertexcount(MAXVERTS)]
void gs_main(point float4 input[1] : SV_Position, uint primId : SV_PrimitiveID, inout ManipulatorStream stream)
{
    float manip_scale = compute_screen_scale();
    uint num = g_Manip.numSegments;

#if MANIP_MODE == 1 && !ELEM_LINES // Translate manip (triangles)
    uint axis = primId / num;
    uint segment = primId % num;
    uint widgetId = widget_axis_x + axis;
    if (is_widget_visible(widgetId))
        emit_cone_segment(stream, axis, segment, manip_scale, widgetId); // 4 verts

    if (primId >= 3)
        return;

    axis = primId;
    widgetId = widget_plane_xy + axis;
    if (is_widget_visible(widgetId))
        emit_planar_square(stream, axis, manip_scale, false, widgetId); // 4 verts

#elif MANIP_MODE == 1 && ELEM_LINES // Translate manip (lines)
    for (uint i = 0; i < 3; ++i)
    {
        uint widgetId = widget_axis_x + i;
        if (is_widget_visible(widgetId))
            emit_line(stream, i, manip_scale, false, widgetId); // 3 * 2 verts
    }
    for (uint j = 0; j < 3; ++j)
    {
        uint widgetId = widget_plane_xy + j;
        if (is_widget_visible(widgetId))
            emit_planar_square_outline(stream, j, manip_scale, false, widgetId); // 3 * 5 verts
    }
    if (primId > 0)
        return;
    emit_billboard_square_outline(stream, manip_scale, widget_plane_view); // 5 verts

#elif MANIP_MODE == 2 && !ELEM_LINES // Rotate manip (triangles)
    // The camembert fan only conveys the in-progress rotation, so draw it only while dragging
    // (rotationArc.w marks the arc valid, set by FillConstants for a live rotate drag).
    if (g_Manip.rotationArc[0].w > 0.0)
    {
        uint segment = primId % num;
        emit_camembert_segment(stream, segment, manip_scale, widget_none); // 4 verts
    }

#elif MANIP_MODE == 2 && ELEM_LINES // Rotate manip (lines)
    uint axis = primId / num;
    uint segment = primId % num;
    emit_circle_segment(stream, axis, segment, 1.0, manip_scale, widget_plane_yz - axis); // 5 verts

    if (axis > 0)
        return;

    emit_billboard_circle_segment(stream, segment, 1.2, manip_scale, widget_plane_view); // 5 verts
    emit_billboard_circle_segment(stream, segment, 1.0, manip_scale, widget_undefined); // 5 verts

    if (primId > 0)
        return;

    if (g_Manip.rotationArc[0].w > 0.0)
    {
        emit_1_line_strip(stream, g_Manip.translate.xyz, g_Manip.translate.xyz + manip_scale * g_Manip.rotationArc[0].xyz, widget_none, 0.75); // 2 verts
        emit_1_line_strip(stream, g_Manip.translate.xyz, g_Manip.translate.xyz + manip_scale * g_Manip.rotationArc[1].xyz, widget_none, 0.75); // 2 verts
    }

#elif MANIP_MODE == 3 && !ELEM_LINES // Scale manip (triangles)
    uint axis = primId / 6;
    uint face = primId % 6;

    uint widgetId = widget_axis_x + axis;
    if (is_widget_visible(widgetId))
    {
        float3 offset = float3(0.0, 0.0, 0.0);
        offset[axis] = 1.0;
        emit_cube_face(stream, face, offset * g_Manip.scale.xyz, manip_scale, true, widgetId); // 4 verts
    }

    if (axis > 1)
        return;

    emit_cube_face(stream, face, float3(0.0, 0.0, 0.0), manip_scale, true, widget_plane_view); // 4 verts

    if (primId >= 3)
        return;

    axis = primId;
    widgetId = widget_plane_xy + axis;
    if (is_widget_visible(widgetId))
        emit_planar_square(stream, axis, manip_scale, true, widgetId); // 4 verts

#elif MANIP_MODE == 3 && ELEM_LINES // Scale manip (lines)
    for (uint i = 0; i < 3; ++i)
    {
        uint widgetId = widget_axis_x + i;
        if (is_widget_visible(widgetId))
            emit_line(stream, i, manip_scale, true, widgetId); // 6 verts
    }
    for (uint j = 0; j < 3; ++j)
    {
        uint widgetId = widget_plane_xy + j;
        if (is_widget_visible(widgetId))
            emit_planar_square_outline(stream, j, manip_scale, true, widgetId); // 6 verts
    }
#endif
}

#endif // MANIPULATOR_STAGE

// Pixel Shader

// the pick variant writes the widget id
#if PICK_PASS
// Widget ids fit in a byte (Manipulator::Widget is uint8_t), so the pick target is R8_UINT.
uint ps_main(PSIn i) : SV_Target0
{
    if (!widget_is_interactive(i.widgetId))
        discard;
    return i.widgetId;
}
#else
float4 ps_main(PSIn i) : SV_Target0
{
    uint color_index = widget_selected_color;
#if MANIP_MODE == 2
    color_index = get_widget_color_index(i.widgetId);
#else
    if (!g_Manip.isDragging)
        color_index = get_widget_color_index(i.widgetId);
#endif
    float3 widget_color = color_table[color_index];

#if !ELEM_LINES
    float alpha = (!widget_is_interactive(i.widgetId) || is_planar_widget(i.widgetId)) ? 0.25 : 1.0;
#else
    float alpha = 1.0;
#endif
    return float4(clamp(widget_color + 0.25 * i.diffuse, 0.0, 1.0), alpha);
}
#endif // PICK_PASS
