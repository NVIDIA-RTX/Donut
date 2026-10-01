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
using namespace donut::render::detail;

namespace
{
    struct DepthInputConfiguration
    {
        using PushConstants = DepthPushConstants;
        static constexpr uint32_t InputSpace = DEPTH_SPACE_INPUT;
        static constexpr uint32_t PushConstantBinding = DEPTH_BINDING_PUSH_CONSTANTS;
        static constexpr uint32_t InstanceBinding = DEPTH_BINDING_INSTANCE_BUFFER;
        static constexpr uint32_t VertexBinding = DEPTH_BINDING_VERTEX_BUFFER;
        static constexpr nvrhi::ShaderType Visibility = nvrhi::ShaderType::Vertex;
        static constexpr bool HasNormals = false;
        static constexpr bool HasPrevPosition = false;
    };
}

template<GeometryInputPolicy InputPolicy>
DepthPassT<InputPolicy>::DepthPassT(
    nvrhi::IDevice* device,
    std::shared_ptr<CommonRenderPasses> commonPasses)
    : m_Device(device)
    , m_CommonPasses(std::move(commonPasses))
{
    m_IsDX11 = m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
}

template<GeometryInputPolicy InputPolicy>
void DepthPassT<InputPolicy>::Init(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    m_UseInputAssembler = params.useInputAssembler;

    m_Input.template Init<DepthInputConfiguration>(m_UseInputAssembler, [&]()
    {
        std::vector<ShaderMacro> macros = { { "DECODE_TEXCOORD", "0" } };
        return shaderFactory.CreateAutoShader("donut/passes/depth_vs.hlsl", "input_assembler",
            DONUT_MAKE_PLATFORM_SHADER(g_depth_vs_input_assembler), &macros, nvrhi::ShaderType::Vertex);
    });

    m_VertexShader = CreateVertexShader(shaderFactory, params);
    m_PixelShader = CreatePixelShader(shaderFactory, params);
    m_InputLayouts = {};
    m_InputLayouts[size_t(TexCoordFormat::Float32)] = CreateInputLayout(
        m_Input.UsesSpecializedInput() ? m_Input.floatVertexShader : m_VertexShader, params);
    m_CreateParameters = params;
    if constexpr (InputPolicy == GeometryInputPolicy::Stock)
        m_InputBindingLayout = GeometryPassInput<InputPolicy>::template CreateBindingLayout<DepthInputConfiguration>(
            m_Device, m_UseInputAssembler, m_IsDX11);
    else
        m_InputBindingLayout = CreateInputBindingLayout();
    m_Input.template InitBindingSet<DepthInputConfiguration>(m_Device, m_InputBindingLayout);

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

template<GeometryInputPolicy InputPolicy>
void DepthPassT<InputPolicy>::ResetBindingCache()
{
    m_MaterialBindings->Clear();
    m_Input.ResetBindingCache();
}

template<GeometryInputPolicy InputPolicy>
nvrhi::ShaderHandle DepthPassT<InputPolicy>::CreateVertexShader(ShaderFactory& shaderFactory, const CreateParameters& params)
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

template<GeometryInputPolicy InputPolicy>
nvrhi::ShaderHandle DepthPassT<InputPolicy>::CreatePixelShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    return shaderFactory.CreateAutoShader("donut/passes/depth_ps.hlsl", "main", DONUT_MAKE_PLATFORM_SHADER(g_depth_ps), nullptr, nvrhi::ShaderType::Pixel);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::InputLayoutHandle DepthPassT<InputPolicy>::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params)
{
    return CreateInputLayout(vertexShader, params, TexCoordFormat::Float32);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::InputLayoutHandle DepthPassT<InputPolicy>::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params, TexCoordFormat texCoordFormat)
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

template<GeometryInputPolicy InputPolicy>
void DepthPassT<InputPolicy>::CreateViewBindings(nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set, const CreateParameters& params)
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

template<GeometryInputPolicy InputPolicy>
std::shared_ptr<MaterialBindingCache> DepthPassT<InputPolicy>::CreateMaterialBindingCache(CommonRenderPasses& commonPasses)
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

template<GeometryInputPolicy InputPolicy>
nvrhi::GraphicsPipelineHandle DepthPassT<InputPolicy>::CreateGraphicsPipeline(PipelineKey key,
    nvrhi::FramebufferInfo const& framebufferInfo)
{
    const auto texCoordFormat = static_cast<TexCoordFormat>(key.bits.texCoordFormat);
    const bool floatingInput = m_Input.UsesSpecializedInput() && texCoordFormat != TexCoordFormat::Unorm16;
    const auto& vertexShader = floatingInput ? m_Input.floatVertexShader : m_VertexShader;
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

    pipelineDesc.bindingLayouts.push_back(floatingInput ? m_Input.floatBindingLayout : m_InputBindingLayout);

    return m_Device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingLayoutHandle DepthPassT<InputPolicy>::CreateInputBindingLayout()
{
    return GeometryPassInput<InputPolicy>::template CreateBindingLayout<DepthInputConfiguration>(
        m_Device, m_UseInputAssembler, m_IsDX11);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingSetHandle DepthPassT<InputPolicy>::CreateInputBindingSet(const BufferGroup* bufferGroup)
{
    return GeometryPassInput<InputPolicy>::template CreateBindingSet<DepthInputConfiguration>(
        m_Device, m_InputBindingLayout, bufferGroup, m_UseInputAssembler, m_IsDX11);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingSetHandle DepthPassT<InputPolicy>::GetOrCreateInputBindingSet(const BufferGroup* bufferGroup)
{
    return m_Input.GetBindingSet(bufferGroup, [&](const BufferGroup* buffers)
    {
        if constexpr (InputPolicy == GeometryInputPolicy::Stock)
            return GeometryPassInput<InputPolicy>::template CreateBindingSet<DepthInputConfiguration>(
                m_Device, m_InputBindingLayout, buffers, m_UseInputAssembler, m_IsDX11);
        else
            return CreateInputBindingSet(buffers);
    });
}

template<GeometryInputPolicy InputPolicy>
void DepthPassT<InputPolicy>::SetPushConstants(
    donut::render::GeometryPassContext& abstractContext,
    nvrhi::ICommandList* commandList,
    nvrhi::GraphicsState& state,
    nvrhi::DrawArguments& args)
{
    auto& context = static_cast<Context&>(abstractContext);
    m_Input.template SetPushConstants<DepthInputConfiguration>(context, commandList, args, m_UseInputAssembler);
}

template<GeometryInputPolicy InputPolicy>
ViewType::Enum DepthPassT<InputPolicy>::GetSupportedViewTypes() const
{
    return ViewType::PLANAR;
}

template<GeometryInputPolicy InputPolicy>
void DepthPassT<InputPolicy>::SetupView(GeometryPassContext& abstractContext, nvrhi::ICommandList* commandList, const engine::IView* view, const engine::IView* viewPrev)
{
    auto& context = static_cast<Context&>(abstractContext);
    context.pushConstantsValid = false;
    
    DepthPassConstants depthConstants = {};
    depthConstants.matWorldToClip = view->GetViewProjectionMatrix();
    commandList->writeBuffer(m_DepthCB, &depthConstants, sizeof(depthConstants));

    context.keyTemplate.bits.frontCounterClockwise = view->IsMirrored();
    context.keyTemplate.bits.reverseDepth = view->IsReverseDepth();
}

template<GeometryInputPolicy InputPolicy>
bool DepthPassT<InputPolicy>::SetupMaterial(GeometryPassContext& abstractContext, const engine::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state)
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

template<GeometryInputPolicy InputPolicy>
void DepthPassT<InputPolicy>::SetupInputBuffers(GeometryPassContext& abstractContext, const engine::BufferGroup* buffers, nvrhi::GraphicsState& state)
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

template class donut::render::detail::DepthPassT<GeometryInputPolicy::Stock>;
template class donut::render::detail::DepthPassT<GeometryInputPolicy::Custom>;
