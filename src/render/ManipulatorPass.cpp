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

#include <donut/render/ManipulatorPass.h>
#include <donut/render/Manipulator.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/View.h>
#include <nvrhi/utils.h>

#include <cassert>
#include <string>
#include <vector>

using namespace donut::math;
#include <donut/shaders/view_cb.h>
#include <donut/shaders/manipulator_cb.h>

using namespace donut::engine;
using namespace donut::render;

namespace
{
    const char* const c_ShaderFile = "donut/passes/manipulator.hlsl";
    const char* const c_ScanShaderFile = "donut/passes/manipulator_scan_cs.hlsl";

    // Number of procedural points dispatched for each (mode, element) pass. The geometry
    // shader expands each point (by SV_PrimitiveID) into one widget primitive.
    uint32_t PointCount(Manipulator::ManipMode mode, bool lines, uint32_t numSegments)
    {
        switch (mode)
        {
            case Manipulator::ManipMode::Translate: return lines ? 1u : 3u * numSegments;
            case Manipulator::ManipMode::Rotate: return lines ? 3u * numSegments : numSegments;
            case Manipulator::ManipMode::Scale: return lines ? 1u : 18u;
            default: return 0u;
        }
    }

    ManipulatorConstants FillConstants(const Manipulator& manipulator, uint32_t numSegments)
    {
        const Manipulator::ManipMode mode = manipulator.SelectedManip();
        const quat rotation = dm::rotationQuat(manipulator.rotate);
        const float3* arc = manipulator.RotationArc();
        const bool arcValid = (mode == Manipulator::ManipMode::Rotate) && manipulator.IsDragging();

        ManipulatorConstants constants = {};
        constants.translate = float4(manipulator.translate, 0.f);
        constants.rotate = float4(rotation.x, rotation.y, rotation.z, rotation.w);
        constants.scale = float4(manipulator.ScaleDelta(), 0.f);
        constants.rotationArc[0] = float4(arc[0], arcValid ? 1.f : 0.f);
        constants.rotationArc[1] = float4(arc[1], arcValid ? 1.f : 0.f);
        constants.selectedWidget = uint32_t(manipulator.SelectedWidget());
        constants.hoveredWidget = uint32_t(manipulator.HoveredWidget());
        constants.numSegments = numSegments;
        constants.manipScale = manipulator.manipScale;
        constants.isDragging = manipulator.IsDragging() ? 1u : 0u;
        return constants;
    }
}

ManipulatorPass::ManipulatorPass(
    nvrhi::IDevice* device,
    const std::shared_ptr<ShaderFactory>& shaderFactory,
    const PlanarView& view,
    uint32_t maxFramesInFlight)
    : m_Device(device)
{
    m_VertexShader = shaderFactory->CreateShader(c_ShaderFile, "vs_main", nullptr, nvrhi::ShaderType::Vertex);

    for (int mode = 1; mode < kNumModes; ++mode)
    {
        for (int elem = 0; elem < kNumElems; ++elem)
        {
            const std::string modeStr = std::to_string(mode);
            const std::string elemStr = std::to_string(elem);

            std::vector<ShaderMacro> gsMacros = {
                ShaderMacro("MANIPULATOR_STAGE", "1"),
                ShaderMacro("MANIP_MODE", modeStr),
                ShaderMacro("ELEM_LINES", elemStr)
            };
            m_GeometryShaders[mode][elem] = shaderFactory->CreateShader(c_ShaderFile, "gs_main", &gsMacros, nvrhi::ShaderType::Geometry);

            std::vector<ShaderMacro> psMacros = {
                ShaderMacro("MANIP_MODE", modeStr),
                ShaderMacro("ELEM_LINES", elemStr),
                ShaderMacro("PICK_PASS", "0")
            };
            m_PixelShaders[mode][elem] = shaderFactory->CreateShader(c_ShaderFile, "ps_main", &psMacros, nvrhi::ShaderType::Pixel);

            std::vector<ShaderMacro> pickPsMacros = {
                ShaderMacro("MANIP_MODE", modeStr),
                ShaderMacro("ELEM_LINES", elemStr),
                ShaderMacro("PICK_PASS", "1")
            };
            m_PickPixelShaders[mode][elem] = shaderFactory->CreateShader(c_ShaderFile, "ps_main", &pickPsMacros, nvrhi::ShaderType::Pixel);
        }
    }

    nvrhi::BufferDesc viewCBDesc;
    viewCBDesc.byteSize = sizeof(PlanarViewConstants);
    viewCBDesc.debugName = "ManipulatorViewConstants";
    viewCBDesc.isConstantBuffer = true;
    viewCBDesc.isVolatile = true;
    viewCBDesc.maxVersions = engine::c_MaxRenderPassConstantBufferVersions;
    m_ViewCB = device->createBuffer(viewCBDesc);

    nvrhi::BufferDesc manipulatorCBDesc;
    manipulatorCBDesc.byteSize = sizeof(ManipulatorConstants);
    manipulatorCBDesc.debugName = "ManipulatorConstants";
    manipulatorCBDesc.isConstantBuffer = true;
    manipulatorCBDesc.isVolatile = true;
    manipulatorCBDesc.maxVersions = engine::c_MaxRenderPassConstantBufferVersions;
    m_ManipulatorCB = device->createBuffer(manipulatorCBDesc);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(1)
    };
    m_BindingLayout = device->createBindingLayout(layoutDesc);

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_ViewCB),
        nvrhi::BindingSetItem::ConstantBuffer(1, m_ManipulatorCB)
    };
    m_BindingSet = device->createBindingSet(bindingSetDesc, m_BindingLayout);

    m_PickScanPass.Init(device, shaderFactory, maxFramesInFlight);
}

void ManipulatorPass::PickScanPass::Init(
    nvrhi::IDevice* device,
    const std::shared_ptr<ShaderFactory>& shaderFactory,
    uint32_t maxFramesInFlight)
{
    shader = shaderFactory->CreateShader(c_ScanShaderFile, "main", nullptr, nvrhi::ShaderType::Compute);

    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(ManipulatorScanConstants);
    cbDesc.debugName = "ManipulatorScanConstants";
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = engine::c_MaxRenderPassConstantBufferVersions;
    constants = device->createBuffer(cbDesc);

    nvrhi::BufferDesc resultDesc;
    resultDesc.byteSize = 2 * sizeof(uint32_t); // [0] = widget id, [1] = winning sample's squared distance
    resultDesc.format = nvrhi::Format::R32_UINT;
    resultDesc.canHaveUAVs = true;
    resultDesc.canHaveTypedViews = true;
    resultDesc.initialState = nvrhi::ResourceStates::CopySource;
    resultDesc.keepInitialState = true;
    resultDesc.debugName = "ManipulatorScan/Result";
    result = device->createBuffer(resultDesc);

    const uint32_t ringSize = std::max(maxFramesInFlight, 1u) + 1u;

    nvrhi::BufferDesc readbackDesc;
    readbackDesc.byteSize = resultDesc.byteSize;
    readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
    readbackRing.resize(ringSize);
    for (uint32_t slot = 0; slot < ringSize; ++slot)
    {
        readbackDesc.debugName = "ManipulatorScan/Readback" + std::to_string(slot);
        readbackRing[slot] = device->createBuffer(readbackDesc);
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(0)
    };
    bindingLayout = device->createBindingLayout(layoutDesc);

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { bindingLayout };
    pipelineDesc.CS = shader;
    pipeline = device->createComputePipeline(pipelineDesc);
}

void ManipulatorPass::PickScanPass::BindSource(nvrhi::IDevice* device, nvrhi::ITexture* pickTexture)
{
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, constants),
        nvrhi::BindingSetItem::Texture_SRV(0, pickTexture),
        nvrhi::BindingSetItem::TypedBuffer_UAV(0, result)
    };
    bindingSet = device->createBindingSet(setDesc, bindingLayout);
}

void ManipulatorPass::PickScanPass::Dispatch(nvrhi::ICommandList* commandList, int2 pixel, int2 viewportSize)
{
    ManipulatorScanConstants cb = {};
    cb.pixel = pixel;
    cb.scanSize = int2(MANIPULATOR_SCAN_TILE, MANIPULATOR_SCAN_TILE);
    cb.viewportSize = viewportSize;
    commandList->writeBuffer(constants, &cb, sizeof(cb));

    nvrhi::ComputeState state;
    state.pipeline = pipeline;
    state.bindings = { bindingSet };
    commandList->setComputeState(state);
    commandList->dispatch(1, 1, 1);

    // Stream the scan result (the widget ID) into this frame's slot, then bump the count.
    // Read() maps the slot that is now the oldest (copy completed (ring - 1) frames ago)
    const uint32_t slot = uint32_t(framesWritten % readbackRing.size());
    commandList->copyBuffer(readbackRing[slot], 0, result, 0, readbackRing[slot]->getDesc().byteSize);
    ++framesWritten;
}

Manipulator::Widget ManipulatorPass::PickScanPass::Read(nvrhi::IDevice* device)
{
    // Non-blocking contract: read-back from a buffer where GPU copy is guaranteed done (oldest slot in the ring)

    if (framesWritten < readbackRing.size())
        return Manipulator::Widget::None;

    const uint32_t oldest = uint32_t(framesWritten % readbackRing.size());
    if (void* data = device->mapBuffer(readbackRing[oldest], nvrhi::CpuAccessMode::Read))
    {
        const Manipulator::Widget widget = Manipulator::Widget(static_cast<const uint32_t*>(data)[0]);
        device->unmapBuffer(readbackRing[oldest]);
        return widget;
    }
    return Manipulator::Widget::None;
}

const ManipulatorPass::PipelineSet& ManipulatorPass::GetPipelines(const nvrhi::FramebufferInfo& fbInfo, bool reversedDepth)
{
    if (auto it = m_Pipelines.find(fbInfo); it != m_Pipelines.end())
        return it->second;

    bool pick = !fbInfo.colorFormats.empty() && fbInfo.colorFormats[0] == nvrhi::Format::R8_UINT;

    PipelineSet set = {};
    for (int mode = 1; mode < kNumModes; ++mode)
    {
        for (int elem = 0; elem < kNumElems; ++elem)
        {
            nvrhi::GraphicsPipelineDesc pipelineDesc;
            pipelineDesc.primType = nvrhi::PrimitiveType::PointList;
            pipelineDesc.VS = m_VertexShader;
            pipelineDesc.GS = m_GeometryShaders[mode][elem];
            pipelineDesc.PS = pick ? m_PickPixelShaders[mode][elem] : m_PixelShaders[mode][elem];
            pipelineDesc.bindingLayouts = { m_BindingLayout };

            pipelineDesc.renderState.rasterState.setCullNone();

            pipelineDesc.renderState.depthStencilState
                .enableDepthTest()
                .enableDepthWrite()
                .disableStencil()
                .setDepthFunc(reversedDepth ? nvrhi::ComparisonFunc::GreaterOrEqual : nvrhi::ComparisonFunc::LessOrEqual);

            // Color: translucent planar quads (alpha = 0.25 blend over the shaded frame)
            // Pick: raw ids on a UINT target (no blend)
            if (!pick)
            {
                pipelineDesc.renderState.blendState.targets[0]
                    .enableBlend()
                    .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                    .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                    .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                    .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
            }

            set.pipelines[mode][elem] = m_Device->createGraphicsPipeline(pipelineDesc, fbInfo);
        }
    }
    return m_Pipelines.emplace(fbInfo, set).first->second;
}

void ManipulatorPass::Render(
    nvrhi::ICommandList* commandList,
    const PlanarView& view,
    const std::shared_ptr<FramebufferFactory>& framebuffer,
    const Manipulator& manipulator)
{
    Manipulator::ManipMode mode = manipulator.SelectedManip();
    if (mode == Manipulator::ManipMode::None)
        return;

    bool reversedDepth = view.IsReverseDepth();

    nvrhi::utils::ScopedMarker marker(commandList, "Manipulator");

    PlanarViewConstants viewConstants;
    view.FillPlanarViewConstants(viewConstants);
    commandList->writeBuffer(m_ViewCB, &viewConstants, sizeof(viewConstants));

    ManipulatorConstants constants = FillConstants(manipulator, manipulator.numSegments);
    commandList->writeBuffer(m_ManipulatorCB, &constants, sizeof(constants));

    commandList->clearDepthStencilTexture(framebuffer->DepthTarget, nvrhi::AllSubresources, true, reversedDepth ? 0.f : 1.f, true, 0);

    const PipelineSet& pipelines = GetPipelines(framebuffer->GetFramebufferInfo(), reversedDepth);

    nvrhi::GraphicsState state;
    state.framebuffer = framebuffer->GetFramebuffer(view);
    state.bindings = { m_BindingSet };
    state.viewport = view.GetViewportState();

    int modeIndex = int(mode);

    // Rasterize triangles, then lines
    for (int elem = 0; elem < kNumElems; ++elem)
    {
        uint32_t points = PointCount(mode, elem == 1, manipulator.numSegments);
        if (points == 0)
            continue;

        state.pipeline = pipelines.pipelines[modeIndex][elem];
        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.vertexCount = points;
        args.instanceCount = 1;
        commandList->draw(args);
    }
}

bool ManipulatorPass::PickRenderTarget::Resize(nvrhi::IDevice* device, int2 size)
{
    size = max(size, int2(1, 1));

    // Grow-only: keep the targets if they already cover the requested viewport (largest seen so far).
    if (idTexture && int(idTexture->getDesc().width) >= size.x && int(idTexture->getDesc().height) >= size.y)
        return false;

    idTexture = device->createTexture(nvrhi::TextureDesc()
        .setDimension(nvrhi::TextureDimension::Texture2D)
        .setWidth(uint32_t(size.x))
        .setHeight(uint32_t(size.y))
        .setFormat(nvrhi::Format::R8_UINT)
        .setIsRenderTarget(true)
        .setInitialState(nvrhi::ResourceStates::RenderTarget)
        .setKeepInitialState(true)
        .setDebugName("ManipulatorIDs"));

    depth = device->createTexture(nvrhi::TextureDesc()
        .setDimension(nvrhi::TextureDimension::Texture2D)
        .setWidth(uint32_t(size.x))
        .setHeight(uint32_t(size.y))
        .setFormat(nvrhi::Format::D32)
        .setIsRenderTarget(true)
        .setInitialState(nvrhi::ResourceStates::DepthWrite)
        .setKeepInitialState(true)
        .setDebugName("ManipulatorPickDepth"));

    framebuffer = std::make_shared<FramebufferFactory>(device);
    framebuffer->RenderTargets = { idTexture };
    framebuffer->DepthTarget = depth;
    return true;
}

void ManipulatorPass::RenderPick(
    nvrhi::ICommandList* commandList,
    const PlanarView& view,
    const Manipulator& manipulator)
{
    // Strip jitter so the widget ids rasterize at stable pixel centers
    PlanarView pickView = view;
    pickView.SetPixelOffset(float2(0.f));
    pickView.UpdateCache();

    nvrhi::Rect extent = pickView.GetViewExtent();
    if (m_Pick.Resize(m_Device, int2(extent.maxX - extent.minX, extent.maxY - extent.minY)))
    {
        // The pick pass recreated its render target, so rebind the scan source
        m_PickScanPass.BindSource(m_Device, m_Pick.idTexture);
    }

    bool reversedDepth = pickView.IsReverseDepth();

    commandList->clearTextureUInt(m_Pick.idTexture, nvrhi::AllSubresources, uint32_t(Manipulator::Widget::None));
    commandList->clearDepthStencilTexture(m_Pick.depth, nvrhi::AllSubresources, true, reversedDepth ? 0.f : 1.f, false, 0);

    Manipulator::ManipMode mode = manipulator.SelectedManip();
    if (mode == Manipulator::ManipMode::None)
        return;

    nvrhi::utils::ScopedMarker marker(commandList, "ManipulatorPick");

    PlanarViewConstants viewConstants;
    pickView.FillPlanarViewConstants(viewConstants);
    commandList->writeBuffer(m_ViewCB, &viewConstants, sizeof(viewConstants));

    ManipulatorConstants constants = FillConstants(manipulator, manipulator.numSegments);
    commandList->writeBuffer(m_ManipulatorCB, &constants, sizeof(constants));

    const PipelineSet& pipelines = GetPipelines(m_Pick.framebuffer->GetFramebufferInfo(), reversedDepth);

    nvrhi::GraphicsState state;
    state.framebuffer = m_Pick.framebuffer->GetFramebuffer(pickView);
    state.bindings = { m_BindingSet };
    state.viewport = pickView.GetViewportState();

    int modeIndex = int(mode);

    for (int elem = 0; elem < kNumElems; ++elem)
    {
        uint32_t points = PointCount(mode, elem == 1, manipulator.numSegments);
        if (points == 0)
            continue;

        state.pipeline = pipelines.pipelines[modeIndex][elem];
        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.vertexCount = points;
        args.instanceCount = 1;
        commandList->draw(args);
    }
}

void ManipulatorPass::Scan(
    nvrhi::ICommandList* commandList,
    int2 pixel,
    int2 viewportSize)
{
    assert(m_Pick.idTexture && m_PickScanPass.bindingSet
        && "ManipulatorPass::Scan: the pick render targets are uninitialized");

    m_PickScanPass.Dispatch(commandList, pixel, viewportSize);
}

Manipulator::Widget ManipulatorPass::ReadHoveredWidget()
{
    return m_PickScanPass.Read(m_Device);
}
