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

#include <donut/render/DepthPass.h>
#include "TexCoordUtils.h"
#include <donut/render/DrawStrategy.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/View.h>
#include <donut/engine/MaterialBindingCache.h>
#include <nvrhi/utils.h>
#include <utility>

#if DONUT_WITH_STATIC_SHADERS
#if DONUT_WITH_DX11
#include "compiled_shaders/passes/depth_vs_input_assembler.dxbc.h"
#include "compiled_shaders/passes/depth_vs_buffer_loads.dxbc.h"
#include "compiled_shaders/passes/depth_ps.dxbc.h"
#endif
#if DONUT_WITH_DX12
#include "compiled_shaders/passes/depth_vs_input_assembler.dxil.h"
#include "compiled_shaders/passes/depth_vs_buffer_loads.dxil.h"
#include "compiled_shaders/passes/depth_ps.dxil.h"
#endif
#if DONUT_WITH_VULKAN
#include "compiled_shaders/passes/depth_vs_input_assembler.spirv.h"
#include "compiled_shaders/passes/depth_vs_buffer_loads.spirv.h"
#include "compiled_shaders/passes/depth_ps.spirv.h"
#endif
#endif

using namespace donut::math;
#include <donut/shaders/depth_cb.h>


using namespace donut::engine;
using namespace donut::render;

DepthPass::DepthPass(
    nvrhi::IDevice* device,
    std::shared_ptr<CommonRenderPasses> commonPasses)
    : m_Device(device)
    , m_CommonPasses(std::move(commonPasses))
{
    m_IsDX11 = m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
}

void DepthPass::Init(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    m_UseInputAssembler = params.useInputAssembler;

    m_OptimizedFloatVertexShader = nullptr;
    m_UnormInputBindingSet = nullptr;
    m_InputBindingSets.clear();
    for (auto& pipeline : m_Pipelines)
        pipeline = nullptr;

    if (params.enableTexCoordOptimizations && m_UseInputAssembler)
    {
        std::vector<ShaderMacro> macros = { { "DECODE_TEXCOORD", "0" } };
        // A missing optimized shader leaves the generic input path available.
        m_OptimizedFloatVertexShader = shaderFactory.CreateAutoShader("donut/passes/depth_vs.hlsl", "input_assembler",
            DONUT_MAKE_PLATFORM_SHADER(g_depth_vs_input_assembler), &macros, nvrhi::ShaderType::Vertex);
    }

    m_VertexShader = CreateVertexShader(shaderFactory, params);
    m_PixelShader = CreatePixelShader(shaderFactory, params);
    m_InputLayouts = {};
    m_InputLayouts[size_t(TexCoordFormat::Float32)] = CreateInputLayout(
        m_OptimizedFloatVertexShader != nullptr ? m_OptimizedFloatVertexShader : m_VertexShader, params);
    m_CreateParameters = params;
    m_InputBindingLayout = CreateInputBindingLayout();
    if (m_OptimizedFloatVertexShader)
        m_UnormInputBindingSet = DepthPass::CreateInputBindingSet(nullptr);

    if (params.materialBindings)
        m_MaterialBindings = params.materialBindings;
    else
        m_MaterialBindings = CreateMaterialBindingCache(*m_CommonPasses);

    m_DepthCB = m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(DepthPassConstants),
        "DepthPassConstants", params.numConstantBufferVersions));

    CreateViewBindings(m_ViewBindingLayout, m_ViewBindingSet, params);

    m_DepthBias = params.depthBias;
    m_DepthBiasClamp = params.depthBiasClamp;
    m_SlopeScaledDepthBias = params.slopeScaledDepthBias;
}

void DepthPass::ResetBindingCache()
{
    m_MaterialBindings->Clear();
    m_InputBindingSets.clear();
}

nvrhi::ShaderHandle DepthPass::CreateVertexShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    char const* sourceFileName = "donut/passes/depth_vs.hlsl";

    if (params.useInputAssembler)
    {
        std::vector<ShaderMacro> macros = { { "DECODE_TEXCOORD", "1" } };
        return shaderFactory.CreateAutoShader(sourceFileName, "input_assembler",
            DONUT_MAKE_PLATFORM_SHADER(g_depth_vs_input_assembler), &macros, nvrhi::ShaderType::Vertex);
    }
    else
    {
        return shaderFactory.CreateAutoShader(sourceFileName, "buffer_loads",
            DONUT_MAKE_PLATFORM_SHADER(g_depth_vs_buffer_loads), nullptr, nvrhi::ShaderType::Vertex);
    }
}

nvrhi::ShaderHandle DepthPass::CreatePixelShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    return shaderFactory.CreateAutoShader("donut/passes/depth_ps.hlsl", "main", DONUT_MAKE_PLATFORM_SHADER(g_depth_ps), nullptr, nvrhi::ShaderType::Pixel);
}

nvrhi::InputLayoutHandle DepthPass::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params)
{
    return CreateInputLayout(vertexShader, params, TexCoordFormat::Float32);
}

nvrhi::InputLayoutHandle DepthPass::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params, TexCoordFormat texCoordFormat)
{
    if (params.useInputAssembler)
    {
        nvrhi::VertexAttributeDesc aInputDescs[] =
        {
            GetVertexAttributeDesc(VertexAttribute::Position, "POSITION", 0),
            GetVertexAttributeDesc(VertexAttribute::TexCoord1, "TEXCOORD", 1, texCoordFormat),
            GetVertexAttributeDesc(VertexAttribute::Transform, "TRANSFORM", 2)
        };

        return m_Device->createInputLayout(aInputDescs, dim(aInputDescs), vertexShader);
    }

    return nullptr;
}

void DepthPass::CreateViewBindings(nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set, const CreateParameters& params)
{
    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .setRegisterSpaceAndDescriptorSet(DEPTH_SPACE_VIEW)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(DEPTH_BINDING_VIEW_CONSTANTS))
        .addItem(nvrhi::BindingLayoutItem::Sampler(DEPTH_BINDING_MATERIAL_SAMPLER));

    layout = m_Device->createBindingLayout(bindingLayoutDesc);

    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .setTrackLiveness(params.trackLiveness)
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(DEPTH_BINDING_VIEW_CONSTANTS, m_DepthCB))
        .addItem(nvrhi::BindingSetItem::Sampler(DEPTH_BINDING_MATERIAL_SAMPLER,
            m_CommonPasses->m_AnisotropicWrapSampler));

    set = m_Device->createBindingSet(bindingSetDesc, layout);
}

std::shared_ptr<MaterialBindingCache> DepthPass::CreateMaterialBindingCache(CommonRenderPasses& commonPasses)
{
    std::vector<MaterialResourceBinding> materialBindings = {
        { MaterialResource::DiffuseTexture, DEPTH_BINDING_MATERIAL_DIFFUSE_TEXTURE },
        { MaterialResource::OpacityTexture, DEPTH_BINDING_MATERIAL_OPACITY_TEXTURE },
        { MaterialResource::ConstantBuffer, DEPTH_BINDING_MATERIAL_CONSTANTS }
    };

    return std::make_shared<MaterialBindingCache>(
        m_Device,
        nvrhi::ShaderType::Pixel,
        /* registerSpace = */ DEPTH_SPACE_MATERIAL,
        /* registerSpaceIsDescriptorSet = */ true,
        materialBindings,
        commonPasses.m_AnisotropicWrapSampler,
        commonPasses.m_GrayTexture,
        commonPasses.m_BlackTexture);
}

nvrhi::GraphicsPipelineHandle DepthPass::CreateGraphicsPipeline(PipelineKey key,
    nvrhi::FramebufferInfo const& framebufferInfo)
{
    const auto texCoordFormat = static_cast<TexCoordFormat>(key.bits.texCoordFormat);
    const bool floatingInput = m_OptimizedFloatVertexShader != nullptr && texCoordFormat != TexCoordFormat::Unorm16;
    const auto& vertexShader = floatingInput ? m_OptimizedFloatVertexShader : m_VertexShader;
    if (size_t(texCoordFormat) >= m_InputLayouts.size())
        return nullptr;
    auto& inputLayout = m_InputLayouts[size_t(texCoordFormat)];

    if (texCoordFormat != TexCoordFormat::Float32 && !inputLayout)
    {
        inputLayout = CreateInputLayout(vertexShader, m_CreateParameters, texCoordFormat);
        if (!inputLayout)
            return nullptr;
    }

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.inputLayout = inputLayout;
    pipelineDesc.VS = vertexShader;
    pipelineDesc.PS = nullptr;
    pipelineDesc.renderState.rasterState.depthBias = m_DepthBias;
    pipelineDesc.renderState.rasterState.depthBiasClamp = m_DepthBiasClamp;
    pipelineDesc.renderState.rasterState.slopeScaledDepthBias = m_SlopeScaledDepthBias;
    pipelineDesc.renderState.rasterState.frontCounterClockwise = key.bits.frontCounterClockwise;
    pipelineDesc.renderState.rasterState.cullMode = key.bits.cullMode;
    pipelineDesc.renderState.depthStencilState.depthFunc = key.bits.reverseDepth
        ? nvrhi::ComparisonFunc::GreaterOrEqual
        : nvrhi::ComparisonFunc::LessOrEqual;

    pipelineDesc.bindingLayouts = { m_ViewBindingLayout };

    if (key.bits.alphaTested)
    {
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts.push_back(m_MaterialBindings->GetLayout());
    }

    if (!floatingInput)
        pipelineDesc.bindingLayouts.push_back(m_InputBindingLayout);

    return m_Device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
}

nvrhi::BindingLayoutHandle DepthPass::CreateInputBindingLayout()
{
    auto desc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex)
        .setRegisterSpaceAndDescriptorSet(DEPTH_SPACE_INPUT)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(DEPTH_BINDING_PUSH_CONSTANTS, sizeof(DepthPushConstants)));
    if (!m_UseInputAssembler)
        desc.addItem(m_IsDX11
            ? nvrhi::BindingLayoutItem::RawBuffer_SRV(DEPTH_BINDING_INSTANCE_BUFFER)
            : nvrhi::BindingLayoutItem::StructuredBuffer_SRV(DEPTH_BINDING_INSTANCE_BUFFER))
            .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(DEPTH_BINDING_VERTEX_BUFFER));
    return m_Device->createBindingLayout(desc);
}

nvrhi::BindingSetHandle DepthPass::CreateInputBindingSet(const BufferGroup* bufferGroup)
{
    auto desc = nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(
        DEPTH_BINDING_PUSH_CONSTANTS, sizeof(DepthPushConstants)));
    if (!m_UseInputAssembler)
        desc.addItem(m_IsDX11
            ? nvrhi::BindingSetItem::RawBuffer_SRV(DEPTH_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer)
            : nvrhi::BindingSetItem::StructuredBuffer_SRV(DEPTH_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer))
            .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(DEPTH_BINDING_VERTEX_BUFFER, bufferGroup->vertexBuffer));
    return m_Device->createBindingSet(desc, m_InputBindingLayout);
}

nvrhi::BindingSetHandle DepthPass::GetOrCreateInputBindingSet(const BufferGroup* bufferGroup)
{
    // nullptr means the optimized FP32/FP16 path needs no additional input binding.
    if (m_OptimizedFloatVertexShader)
        return bufferGroup->texCoordFormat == TexCoordFormat::Unorm16 ? m_UnormInputBindingSet : nullptr;

    auto it = m_InputBindingSets.find(bufferGroup);
    if (it == m_InputBindingSets.end())
    {
        auto bindings = CreateInputBindingSet(bufferGroup);
        m_InputBindingSets[bufferGroup] = bindings;
        return bindings;
    }
    return it->second;
}

void DepthPass::SetPushConstants(
    donut::render::GeometryPassContext& abstractContext,
    nvrhi::ICommandList* commandList,
    nvrhi::GraphicsState& state,
    nvrhi::DrawArguments& args)
{
    auto& context = static_cast<Context&>(abstractContext);
    if (m_OptimizedFloatVertexShader)
    {
        if (context.texCoordFormat != TexCoordFormat::Unorm16)
            return;
        DepthPushConstants constants = {};
        constants.texCoordFormat = uint32_t(TexCoordFormat::Unorm16);
        constants.texCoordScaleBias = detail::GetTexCoordScaleBias(
            context.texCoordFormat, context.inputBuffers, context.geometry, args.startVertexLocation);
        if (!detail::UpdateTexCoordScaleBiasCache(constants.texCoordScaleBias, context.lastTexCoordScaleBias,
            context.enablePushConstantCaching, context.pushConstantsValid))
            return;
        commandList->setPushConstants(&constants, sizeof(constants));
        return;
    }

    DepthPushConstants constants = {};
    constants.startInstanceLocation = args.startInstanceLocation;
    constants.startVertexLocation = args.startVertexLocation;
    constants.positionOffset = context.positionOffset;
    constants.texCoordOffset = context.texCoordOffset;
    constants.texCoordFormat = uint32_t(context.texCoordFormat);
    constants.texCoordScaleBias = detail::GetTexCoordScaleBias(
        context.texCoordFormat, context.inputBuffers, context.geometry, args.startVertexLocation);
    commandList->setPushConstants(&constants, sizeof(constants));
    if (!m_UseInputAssembler)
    {
        args.startInstanceLocation = 0;
        args.startVertexLocation = 0;
    }
}

ViewType::Enum DepthPass::GetSupportedViewTypes() const
{
    return ViewType::PLANAR;
}

void DepthPass::SetupView(GeometryPassContext& abstractContext, nvrhi::ICommandList* commandList, const engine::IView* view, const engine::IView* viewPrev)
{
    auto& context = static_cast<Context&>(abstractContext);
    context.pushConstantsValid = false;
    
    DepthPassConstants depthConstants = {};
    depthConstants.matWorldToClip = view->GetViewProjectionMatrix();
    commandList->writeBuffer(m_DepthCB, &depthConstants, sizeof(depthConstants));

    context.keyTemplate.bits.frontCounterClockwise = view->IsMirrored();
    context.keyTemplate.bits.reverseDepth = view->IsReverseDepth();
}

bool DepthPass::SetupMaterial(GeometryPassContext& abstractContext, const engine::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state)
{
    auto& context = static_cast<Context&>(abstractContext);

    PipelineKey key = context.keyTemplate;
    key.bits.cullMode = cullMode;

    bool const hasBaseOrDiffuseTexture = material->baseOrDiffuseTexture
        && material->baseOrDiffuseTexture->texture
        && material->enableBaseOrDiffuseTexture;

    bool const hasOpacityTexture = material->opacityTexture
        && material->opacityTexture->texture
        && material->enableOpacityTexture;
        
    if (material->domain == MaterialDomain::AlphaTested && (hasBaseOrDiffuseTexture || hasOpacityTexture))
    {
        nvrhi::IBindingSet* materialBindingSet = m_MaterialBindings->GetMaterialBindingSet(material);

        if (!materialBindingSet)
            return false;
        
        state.bindings = { m_ViewBindingSet, materialBindingSet };
        key.bits.alphaTested = true;
    }
    else if (material->domain == MaterialDomain::Opaque)
    {
        state.bindings = { m_ViewBindingSet };
        key.bits.alphaTested = false;
    }
    else
    {
        return false;
    }
    
    if (context.inputBindingSet)
        state.bindings.push_back(context.inputBindingSet);

    nvrhi::FramebufferInfo const& framebufferInfo = state.framebuffer->getFramebufferInfo();
    nvrhi::GraphicsPipelineHandle& pipeline = m_Pipelines[key.value];

    if (!pipeline)
    {
        std::lock_guard<std::mutex> lockGuard(m_Mutex);

        if (!pipeline)
            pipeline = CreateGraphicsPipeline(key, framebufferInfo);

        if (!pipeline)
            return false;
    }

    assert(pipeline->getFramebufferInfo() == framebufferInfo);

    state.pipeline = pipeline;
    return true;
}

void DepthPass::SetupInputBuffers(GeometryPassContext& abstractContext, const engine::BufferGroup* buffers, nvrhi::GraphicsState& state)
{
    auto& context = static_cast<Context&>(abstractContext);

    context.inputBuffers = buffers;
    context.inputBindingSet = GetOrCreateInputBindingSet(buffers);
    context.texCoordFormat = buffers->texCoordFormat;
    context.keyTemplate.bits.texCoordFormat = uint8_t(m_UseInputAssembler ? buffers->texCoordFormat : TexCoordFormat::Float32);

    state.indexBuffer = { buffers->indexBuffer, nvrhi::Format::R32_UINT, 0 };

    if (m_UseInputAssembler)
    {
        state.vertexBuffers = {
            { buffers->vertexBuffer, 0, buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset },
            { buffers->vertexBuffer, 1, buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset },
            { buffers->instanceBuffer, 2, 0 }
        };
    }
    else
    {
        context.positionOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset);
        context.texCoordOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
    }
}
