/*
* Copyright (c) 2014-2026, NVIDIA CORPORATION. All rights reserved.
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

#include <donut/render/Manipulator.h>
#include <donut/engine/View.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

using namespace donut::math;

namespace donut::render
{
namespace
{
    constexpr float eps = 1e-6f;

    struct Ray
    {
        float3 orig;
        float3 dir;
    };

    struct Plane
    {
        float3 orig;
        float3 normal;
    };

    std::optional<std::pair<float3, float3>> closestPoints(const Ray& r0, const Ray& r1)
    {
        float3 v01 = r0.orig - r1.orig;
        float r00 = dot(r0.dir, r0.dir);
        float r11 = dot(r1.dir, r1.dir);
        float r01 = dot(r0.dir, r1.dir);
        float p01v0 = dot(v01, r0.dir);
        float p01v1 = dot(v01, r1.dir);

        float denom = r00 * r11 - r01 * r01;
        if (std::fabs(denom) > eps)
        {
            float s = (r01 * p01v1 - r11 * p01v0) / denom;
            float t = (r00 * p01v1 - r01 * p01v0) / denom;
            return std::pair<float3, float3>{ r0.orig + s * r0.dir, r1.orig + t * r1.dir };
        }
        return {};
    }

    std::optional<float3> intersect(const Ray& ray, const Plane& plane)
    {
        const float3 p = plane.orig - ray.orig;
        const float denom = dot(ray.dir, plane.normal);
        if (std::fabs(denom) > eps)
        {
            const float t = dot(p, plane.normal) / denom;
            if (t > 0.f)
                return ray.orig + ray.dir * t;
        }
        return {};
    }

    using Widget = Manipulator::Widget;

    bool isAxis(Widget widget)
    {
        return widget == Widget::AxisX || widget == Widget::AxisY || widget == Widget::AxisZ;
    }

    int axisIndex(Widget widget)
    {
        switch (widget)
        {
            case Widget::AxisX: return 0;
            case Widget::AxisY: return 1;
            case Widget::AxisZ: return 2;
            default: return 0;
        }
    }

    float3 axisVector(Widget widget)
    {
        float3 axis = 0.f;
        axis[axisIndex(widget)] = 1.f;
        return axis;
    }

    bool isPlane(Widget widget)
    {
        return widget == Widget::PlaneXY || widget == Widget::PlaneXZ || widget == Widget::PlaneYZ;
    }

    // Index of the axis orthogonal to the plane (the plane's normal axis).
    int planeIndex(Widget widget)
    {
        switch (widget)
        {
            case Widget::PlaneXY: return 2;
            case Widget::PlaneXZ: return 1;
            case Widget::PlaneYZ: return 0;
            default: return 0;
        }
    }

    float3 planeAxis(Widget widget)
    {
        float3 axis = 0.f;
        axis[planeIndex(widget)] = 1.f;
        return axis;
    }

    Plane cardinalPlane(Widget widget)
    {
        int i = planeIndex(widget);
        Plane plane = { float3(0.5f), float3(0.f) };
        plane.orig[i] = 0.f;
        plane.normal[i] = 1.f;
        return plane;
    }
}

float3 Manipulator::computeRayDir(float2 pixel) const
{
    // Pixel (top-left origin) -> D3D NDC (y up)
    float2 ndc = float2(
        2.f * (pixel.x / m_viewportSize.x) - 1.f,
        1.f - 2.f * (pixel.y / m_viewportSize.y));
    float4 world = float4(ndc.x, ndc.y, 0.5f, 1.f) * m_invViewProj;
    float3 worldPos = float3(world.x, world.y, world.z) / world.w;
    return normalize(worldPos - m_cameraEye);
}

bool Manipulator::applyTranslate(float2 currPos)
{
    auto axisTranslation = [this](const float3& axis, float2 cur) -> float3 {
        Ray r0 = { m_startTranslate, axis };
        Ray r1 = { m_cameraEye, computeRayDir(m_startPos) };
        Ray r2 = { m_cameraEye, computeRayDir(cur) };

        auto pair0 = closestPoints(r0, r1);
        auto pair1 = closestPoints(r0, r2);
        if (pair0 && pair1)
            return m_startTranslate + axis * (pair1->first - pair0->first);
        return m_startTranslate;
    };
    auto planarTranslation = [this](const Plane& plane, float2 cur) -> float3 {
        Ray r0 = { m_cameraEye, computeRayDir(m_startPos) };
        Ray r1 = { m_cameraEye, computeRayDir(cur) };
        
        auto p0 = intersect(r0, plane);
        auto p1 = intersect(r1, plane);
        if (p0 && p1)
            return m_startTranslate + (*p1 - *p0);
        return m_startTranslate;
    };

    if (isAxis(m_selectedWidget))
        translate = axisTranslation(axisVector(m_selectedWidget), currPos);
    else if (isPlane(m_selectedWidget))
        translate = planarTranslation(cardinalPlane(m_selectedWidget), currPos);
    else if (m_selectedWidget == Widget::PlaneView)
        translate = planarTranslation(Plane{ m_startTranslate, m_cameraForward }, currPos);
    else
        return false;
    return true;
}

bool Manipulator::applyRotate(float2 currPos)
{
    if (!(isPlane(m_selectedWidget) || m_selectedWidget == Widget::PlaneView))
        return false;

    quat startRot = rotationQuat(m_startRotate);

    Plane rotPlane = { translate, isPlane(m_selectedWidget) ?
        applyQuat(startRot, planeAxis(m_selectedWidget)) : normalize(m_cameraEye - translate)
    };
    assert(std::fabs(length(rotPlane.normal) - 1) < eps);

    Ray r0 = { m_cameraEye, computeRayDir(m_startPos) };
    Ray r1 = { m_cameraEye, computeRayDir(currPos) };

    auto p0 = intersect(r0, rotPlane);
    auto p1 = intersect(r1, rotPlane);
    if (p0 && p1)
    {
        float3 u = normalize(*p0 - translate);
        float3 v = normalize(*p1 - translate);
        float3 w = cross(u, v);

        if (const float len = length(w); len > 0.f)
        {
            const float angle = std::copysign(std::acos(clamp(dot(u, v), -1.f, 1.f)), dot(w / len, rotPlane.normal));
            rotate = eulerFromQuat(rotationQuat(rotPlane.normal, angle) * startRot);
        }
        m_rotationArc[0] = u;
        m_rotationArc[1] = v;
    }
    else
    {
        rotate = m_startRotate;
    }
    return true;
}

bool Manipulator::applyScale(float2 currPos)
{
    auto computeDelta = [this](float3 p0, float3 p1) -> float {
        float3 v0 = p0 - translate;
        float3 v1 = p1 - translate;
        return std::copysign(length(v1) / length(v0), dot(v0, v1));
    };
    auto axisScale = [this, &computeDelta](Widget widget, float2 cur) -> bool {
        float3 axis = axisVector(widget);
        Ray r0 = { m_startTranslate, axis };
        Ray r1 = { m_cameraEye, computeRayDir(m_startPos) };
        Ray r2 = { m_cameraEye, computeRayDir(cur) };

        auto pair0 = closestPoints(r0, r1);
        auto pair1 = closestPoints(r0, r2);
        if (pair0 && pair1)
        {
            m_scaleDelta[axisIndex(widget)] = computeDelta(pair0->first, pair1->first);
            return true;
        }
        return false;
    };
    auto planarScale = [this, &computeDelta](Widget widget, float2 cur) -> bool {
        Plane plane = cardinalPlane(widget);
        Ray r0 = { m_cameraEye, computeRayDir(m_startPos) };
        Ray r1 = { m_cameraEye, computeRayDir(cur) };

        auto p0 = intersect(r0, plane);
        auto p1 = intersect(r1, plane);
        if (p0 && p1)
        {
            float delta = computeDelta(*p0, *p1);
            m_scaleDelta = float3(delta);
            m_scaleDelta[planeIndex(widget)] = 1.f;
            return true;
        }
        return false;
    };
    auto viewScale = [this](float2 cur) -> bool {
        float delta = (manipScale + (cur.x - m_startPos.x)) / manipScale;
        m_scaleDelta = float3(delta);
        return true;
    };

    m_scaleDelta = 1.f;

    bool applied = false;
    if (isAxis(m_selectedWidget))
        applied = axisScale(m_selectedWidget, currPos);
    else if (isPlane(m_selectedWidget))
        applied = planarScale(m_selectedWidget, currPos);
    else if (m_selectedWidget == Widget::PlaneView)
        applied = viewScale(currPos);

    if (applied)
        scale = m_startScale * m_scaleDelta;
    return applied;
}

bool Manipulator::StartDrag(const engine::PlanarView& view, int2 pixelPos, Widget widget)
{
    if (!Active() || IsDragging())
        return false;

    SelectWidget(widget);

    m_rotationArc[0] = m_rotationArc[1] = float3(0.f);

    const nvrhi::Viewport& viewport = view.GetViewport();
    m_viewportSize = float2(viewport.width(), viewport.height());
    m_viewportOrigin = float2(viewport.minX, viewport.minY);
    m_cameraEye = view.GetViewOrigin();
    m_invViewProj = view.GetInverseViewProjectionMatrix(false);
    m_cameraForward = computeRayDir(m_viewportSize * 0.5f);

    m_startTranslate = translate;
    m_startRotate = rotate;
    m_startScale = scale;

    m_latestPos = pixelPos;
    m_startPos = float2(float(pixelPos.x), float(pixelPos.y)) - m_viewportOrigin;

    m_isDragging = true;
    SetUpdate();
    return true;
}

void Manipulator::Drag(int2 pixelPos)
{
    if (!m_isDragging || m_selectedWidget == Widget::None)
        return;

    if (pixelPos.x == m_latestPos.x && pixelPos.y == m_latestPos.y)
        return;

    float2 currPos = float2(float(pixelPos.x), float(pixelPos.y)) - m_viewportOrigin;

    bool changed = false;
    switch (m_selectedManip)
    {
        case ManipMode::Translate: changed = applyTranslate(currPos); break;
        case ManipMode::Rotate: changed = applyRotate(currPos); break;
        case ManipMode::Scale: changed = applyScale(currPos); break;
        default: break;
    }
    if (changed)
    {
        if (updateCallback)
            updateCallback(*this);
        SetUpdate();
    }
    m_latestPos = pixelPos;
}

void Manipulator::EndDrag()
{
    if (m_isDragging)
    {
        m_scaleDelta = 1.f;
        m_rotationArc[0] = m_rotationArc[1] = float3(0.f);
        SelectWidget(Widget::None);
    }
    m_isDragging = false;
}

void Manipulator::SelectManip(ManipMode manip)
{
    m_selectedManip = manip;
    m_selectedWidget = Widget::None;
    SetUpdate();
}

void Manipulator::ToggleManip(ManipMode manip)
{
    SelectManip(m_selectedManip == manip ? ManipMode::None : manip);
}

void Manipulator::SelectWidget(Widget widget)
{
    m_selectedWidget = widget;
    SetUpdate();
}

void Manipulator::ToggleWidget(Widget widget)
{
    SelectWidget(m_selectedWidget == widget ? Widget::None : widget);
}

void Manipulator::UpdateTransform(float3 newTranslate, float3 newRotate, float3 newScale)
{
    bool changed = false;
    if (any(translate != newTranslate)) { translate = newTranslate; changed = true; }
    if (any(rotate != newRotate)) { rotate = newRotate; changed = true; }
    if (any(scale != newScale)) { scale = newScale; changed = true; }
    if (changed)
        SetUpdate();
}

} // end namespace donut::render
