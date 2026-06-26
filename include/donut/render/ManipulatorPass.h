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

#include <nvrhi/nvrhi.h>
#include <donut/core/math/math.h>
#include <donut/render/Manipulator.h>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace donut::engine
{
    class ShaderFactory;
    class FramebufferFactory;
    class PlanarView;
}

namespace donut::render
{
    // Rasterizes a Manipulator into a framebuffer (planar viewport)

    class ManipulatorPass
    {
    public:

        // The pass caches it at construction; if the client switches buffering depth at run-time it is
        // expected to tear down and rebuild render passes (including this ring-buffer).
        // note: maxFramesInFlight sizes a read-back ring-buffer so reads never blocks (see PickScanPass).
        ManipulatorPass(
            nvrhi::IDevice* device,
            const std::shared_ptr<engine::ShaderFactory>& shaderFactory,
            const engine::PlanarView& view,
            uint32_t maxFramesInFlight);

        // Draw the visible widgets of the manipulator into the framebuffer
        // note: clears all of frameBuffer's attachments (depth, stencil, ...)
        void Render(
            nvrhi::ICommandList* commandList,
            const engine::PlanarView& view,
            const std::shared_ptr<engine::FramebufferFactory>& framebuffer,
            const Manipulator& manipulator);

        // Draw the widget ids of the manipulator into the pick buffer (see PickRenderTarget)
        // note: ignores view jitter (PixelOffset)
        void RenderPick(
            nvrhi::ICommandList* commandList,
            const engine::PlanarView& view,
            const Manipulator& manipulator);

        // Dispatch a scan of the pick buffer around the mouse cursor (used to highlight widgets when the mouse
        // hovers over the manipulator)
        void Scan(
            nvrhi::ICommandList* commandList,
            dm::int2 pixel,
            dm::int2 viewportSize);

        // Returns the hovered widget (or None) using a non-blocking read-back staging buffer ; depending on
        // swap-chain buffering depth, the result may be a few frames stale
        Manipulator::Widget ReadHoveredWidget();

    private:
        static constexpr uint8_t kNumModes = 4; // see Manipulator::ManipMode
        static constexpr uint8_t kNumElems = 2; // elem 0 = triangles, elem 1 = lines

        // PSO cache for Manipulator procedural geometry shader variants
        struct PipelineSet { nvrhi::GraphicsPipelineHandle pipelines[kNumModes][kNumElems]; };
        const PipelineSet& GetPipelines(const nvrhi::FramebufferInfo& fbInfo, bool reversedDepth);      
        std::unordered_map<nvrhi::FramebufferInfo, PipelineSet> m_Pipelines;

        nvrhi::ShaderHandle m_VertexShader;
        nvrhi::ShaderHandle m_GeometryShaders[kNumModes][kNumElems];
        nvrhi::ShaderHandle m_PixelShaders[kNumModes][kNumElems];
        nvrhi::ShaderHandle m_PickPixelShaders[kNumModes][kNumElems];

        nvrhi::BufferHandle m_ViewCB;
        nvrhi::BufferHandle m_ManipulatorCB;

        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::BindingSetHandle m_BindingSet;

        // R8_UINT id target + depth (no stencil) to rasterize widget ids
        struct PickRenderTarget
        {
            // Grow-only, no-op otherwise; returns true if the targets were (re)created
            bool Resize(nvrhi::IDevice* device, dm::int2 size);

            nvrhi::TextureHandle idTexture; // R8_UINT widget ids
            nvrhi::TextureHandle depth;
            std::shared_ptr<engine::FramebufferFactory> framebuffer;
        } m_Pick;

        // Compute pass scans the pixels around the mouse pointer 
        struct PickScanPass
        {
            void Init(
                nvrhi::IDevice* device,
                const std::shared_ptr<engine::ShaderFactory>& shaderFactory,
                uint32_t maxFramesInFlight);

            void BindSource(nvrhi::IDevice* device, nvrhi::ITexture* pickTexture);

            void Dispatch(nvrhi::ICommandList* commandList, dm::int2 pixel, dm::int2 viewportSize);

            // Returns the widget ID that covers the most pixels in a small tile
            Manipulator::Widget Read(nvrhi::IDevice* device);

            nvrhi::ShaderHandle shader;
            nvrhi::ComputePipelineHandle pipeline;
            nvrhi::BufferHandle constants;
            nvrhi::BufferHandle result;
            nvrhi::BindingLayoutHandle bindingLayout;
            nvrhi::BindingSetHandle bindingSet;

            // Ring-buffer to cache results for non-blocking reads
            std::vector<nvrhi::BufferHandle> readbackRing;
            uint64_t framesWritten = 0;
        } m_PickScanPass;

        nvrhi::IDevice* m_Device = nullptr;
    };
}
