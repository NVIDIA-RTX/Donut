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

#pragma once

#include <donut/core/math/math.h>
#include <cstdint>
#include <functional>

namespace donut::engine
{
    class PlanarView;
}

namespace donut::render
{
    // Assumes Y-up (donut's default)
    class Manipulator
    {
    public:
        enum class ManipMode : uint8_t { None = 0, Translate, Rotate, Scale };

        // Each manipulator presents multiple widgets: cardinal axes, cardinal planes, or view-plane based operations
        // Note : widget_t values are identified in the pick-pass as a range of reserved pick-ids
        enum class Widget : uint8_t { None = 0, AxisX = 0x1, AxisY = 0x2, AxisZ = 0x3, PlaneXY = 0x4, PlaneXZ = 0x5, PlaneYZ = 0x6, PlaneView = 0x7 };

        // Call SetUpdate() (or UpdateTransform) after modifying parameters.
        dm::float3 translate = 0.f;
        dm::float3 rotate = 0.f; // euler radians
        dm::float3 scale = 1.f;

        // Desired on-screen size of the manipulator, in pixel-ish units (used by view-plane scale).
        float manipScale = 350.f;

        // Tessellation factor for cones / rings / disks of the manipulator widgets
        uint32_t numSegments = 50;

        // Invoked whenever user inputs modify the transform values (translate / rotate / scale)
        std::function<void(const Manipulator&)> updateCallback;

        [[nodiscard]] bool Active() const { return m_selectedManip != ManipMode::None; }


        [[nodiscard]] ManipMode SelectedManip() const { return m_selectedManip; }
        void SelectManip(ManipMode manip);
        void ToggleManip(ManipMode manip);

        [[nodiscard]] Widget SelectedWidget() const { return m_selectedWidget; }
        void SelectWidget(Widget widget);
        void ToggleWidget(Widget widget);

        // Widget under the cursor (from the scan pass); drives the hover highlight color.
        [[nodiscard]] Widget HoveredWidget() const { return m_hoveredWidget; }
        void SetHoveredWidget(Widget widget) { m_hoveredWidget = widget; }

        [[nodiscard]] static bool IsWidget(uint32_t id) { return id <= uint32_t(Widget::PlaneView); }

        // Sets the transform, scheduling a redraw only when something actually changed.
        void UpdateTransform(dm::float3 newTranslate, dm::float3 newRotate, dm::float3 newScale);

        // The manipulator carries its dirty state into the next frame so multi-viewport / multi-UI
        // imgui widgets have a chance to observe the change before it is reconciled.
        [[nodiscard]] bool NeedsUpdate() const { return m_currentFrame - m_updateFrame <= 1; }
        void SetUpdate() { m_updateFrame = m_currentFrame; }

        // pixelPos is in render-target pixels (top-left origin), as delivered by the mouse path.
        bool StartDrag(const engine::PlanarView& view, dm::int2 pixelPos, Widget widget);
        void Drag(dm::int2 pixelPos);
        void EndDrag();
        [[nodiscard]] bool IsDragging() const { return m_isDragging; }

        void SetCurrentFrame(uint64_t currentFrame) { m_currentFrame = currentFrame; }

        // Start/current directions of a pending rotation (unit vectors orthogonal to the
        // rotation axis), used to draw the rotate "camembert" feedback.
        [[nodiscard]] const dm::float3* RotationArc() const { return m_rotationArc; }
        [[nodiscard]] dm::float3 ScaleDelta() const { return m_scaleDelta; }

    private:
        dm::float3 computeRayDir(dm::float2 pixel) const;

        bool applyTranslate(dm::float2 currPos);
        bool applyRotate(dm::float2 currPos);
        bool applyScale(dm::float2 currPos);

        ManipMode m_selectedManip = ManipMode::None;
        Widget m_selectedWidget = Widget::None;
        Widget m_hoveredWidget = Widget::None;

        uint64_t m_currentFrame = 0;
        uint64_t m_updateFrame = 0;

        // transform state on drag start
        dm::float3 m_startTranslate = 0.f;
        dm::float3 m_startRotate = 0.f;
        dm::float3 m_startScale = 1.f;

        dm::float3 m_scaleDelta = 1.f;
        // vectors describing the start and current directions of a pending rotation
        // operation ; both are unit vectors, orthogonal to the rotation axis (useful
        // to draw the "camembert" rotation widget)
        dm::float3 m_rotationArc[2] = { dm::float3(0.f), dm::float3(0.f) };

        // camera/view state captured at drag start (drag beyond viewport borders).
        dm::float2 m_viewportSize = 0.f;
        dm::float2 m_viewportOrigin = 0.f;
        dm::float3 m_cameraEye = 0.f;
        dm::float3 m_cameraForward = 0.f;
        dm::float4x4 m_invViewProj = dm::float4x4::identity();

        // mouse drag
        dm::int2 m_latestPos = 0;
        dm::float2 m_startPos = 0.f;

        bool m_isDragging = false;
    };
}
