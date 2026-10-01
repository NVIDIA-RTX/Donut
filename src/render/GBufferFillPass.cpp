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

#include <donut/render/GBufferFillPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/ShadowMap.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/MaterialBindingCache.h>
#include <donut/core/log.h>
#include <nvrhi/utils.h>
#include <utility>

#if DONUT_WITH_STATIC_SHADERS
#if DONUT_WITH_DX11
#include "compiled_shaders/passes/cubemap_gs.dxbc.h"
#include "compiled_shaders/passes/gbuffer_ps.dxbc.h"
#include "compiled_shaders/passes/gbuffer_vs_input_assembler.dxbc.h"
#include "compiled_shaders/passes/gbuffer_vs_buffer_loads.dxbc.h"
#include "compiled_shaders/passes/material_id_ps.dxbc.h"
#endif
#if DONUT_WITH_DX12
#include "compiled_shaders/passes/cubemap_gs.dxil.h"
#include "compiled_shaders/passes/gbuffer_ps.dxil.h"
#include "compiled_shaders/passes/gbuffer_vs_input_assembler.dxil.h"
#include "compiled_shaders/passes/gbuffer_vs_buffer_loads.dxil.h"
#include "compiled_shaders/passes/material_id_ps.dxil.h"
#endif
#if DONUT_WITH_VULKAN
#include "compiled_shaders/passes/cubemap_gs.spirv.h"
#include "compiled_shaders/passes/gbuffer_ps.spirv.h"
#include "compiled_shaders/passes/gbuffer_vs_input_assembler.spirv.h"
#include "compiled_shaders/passes/gbuffer_vs_buffer_loads.spirv.h"
#include "compiled_shaders/passes/material_id_ps.spirv.h"
#endif
#endif

using namespace donut::math;
#include <donut/shaders/gbuffer_cb.h>

using namespace donut::engine;
using namespace donut::render;
using namespace donut::render::detail;

namespace
{
    struct GBufferInputConfiguration
    {
        using PushConstants = GBufferPushConstants;
        static constexpr uint32_t InputSpace = GBUFFER_SPACE_INPUT;
        static constexpr uint32_t PushConstantBinding = GBUFFER_BINDING_PUSH_CONSTANTS;
        static constexpr uint32_t InstanceBinding = GBUFFER_BINDING_INSTANCE_BUFFER;
        static constexpr uint32_t VertexBinding = GBUFFER_BINDING_VERTEX_BUFFER;
        static constexpr nvrhi::ShaderType Visibility = nvrhi::ShaderType(
            uint32_t(nvrhi::ShaderType::Vertex) | uint32_t(nvrhi::ShaderType::Pixel));
        static constexpr bool HasNormals = true;
        static constexpr bool HasPrevPosition = true;
    };
}

template<GeometryInputPolicy InputPolicy>
GBufferFillPassT<InputPolicy>::GBufferFillPassT(nvrhi::IDevice* device, std::shared_ptr<CommonRenderPasses> commonPasses)
    : m_Device(device)
    , m_CommonPasses(std::move(commonPasses))
{
    m_IsDX11 = m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
}

template<GeometryInputPolicy InputPolicy>
void GBufferFillPassT<InputPolicy>::Init(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    m_EnableMotionVectors = params.enableMotionVectors;
    m_UseInputAssembler = params.useInputAssembler;

    m_SupportedViewTypes = ViewType::PLANAR;
    if (params.enableSinglePassCubemap)
        m_SupportedViewTypes = ViewType::Enum(m_SupportedViewTypes | ViewType::CUBEMAP);
    
    m_Input.template Init<GBufferInputConfiguration>(m_UseInputAssembler, [&]()
    {
        std::vector<ShaderMacro> macros;
        macros.emplace_back("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0");
        macros.emplace_back("DECODE_TEXCOORD", "0");
        return shaderFactory.CreateAutoShader("donut/passes/gbuffer_vs.hlsl", "input_assembler",
            DONUT_MAKE_PLATFORM_SHADER(g_gbuffer_vs_input_assembler), &macros, nvrhi::ShaderType::Vertex);
    });

    m_VertexShader = CreateVertexShader(shaderFactory, params);
    m_InputLayouts = {};
    m_InputLayouts[size_t(TexCoordFormat::Float32)] = CreateInputLayout(
        m_Input.UsesSpecializedInput() ? m_Input.floatVertexShader : m_VertexShader, params);
    m_CreateParameters = params;
    m_GeometryShader = CreateGeometryShader(shaderFactory, params);
    m_PixelShader = CreatePixelShader(shaderFactory, params, false);
    m_PixelShaderAlphaTested = CreatePixelShader(shaderFactory, params, true);

    if (params.materialBindings)
        m_MaterialBindings = params.materialBindings;
    else
        m_MaterialBindings = CreateMaterialBindingCache(*m_CommonPasses);

    m_GBufferCB = m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(GBufferFillConstants),
        "GBufferFillConstants", params.numConstantBufferVersions));

    CreateViewBindings(m_ViewBindingLayout, m_ViewBindings, params);

    m_EnableDepthWrite = params.enableDepthWrite;
    m_StencilWriteMask = params.stencilWriteMask;

    if constexpr (InputPolicy == GeometryInputPolicy::Stock)
        m_InputBindingLayout = GeometryPassInput<InputPolicy>::template CreateBindingLayout<GBufferInputConfiguration>(
            m_Device, m_UseInputAssembler, m_IsDX11);
    else
        m_InputBindingLayout = CreateInputBindingLayout();
    m_Input.template InitBindingSet<GBufferInputConfiguration>(m_Device, m_InputBindingLayout);
}

template<GeometryInputPolicy InputPolicy>
void GBufferFillPassT<InputPolicy>::ResetBindingCache()
{
    m_MaterialBindings->Clear();
    m_Input.ResetBindingCache();
}

template<GeometryInputPolicy InputPolicy>
nvrhi::ShaderHandle GBufferFillPassT<InputPolicy>::CreateVertexShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    char const* sourceFileName = "donut/passes/gbuffer_vs.hlsl";

    std::vector<ShaderMacro> VertexShaderMacros;
    VertexShaderMacros.push_back(ShaderMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0"));

    if (params.useInputAssembler)
    {
        VertexShaderMacros.emplace_back("DECODE_TEXCOORD", "1");
        return shaderFactory.CreateAutoShader(sourceFileName, "input_assembler",
            DONUT_MAKE_PLATFORM_SHADER(g_gbuffer_vs_input_assembler), &VertexShaderMacros, nvrhi::ShaderType::Vertex);
    }
    else
    {
        return shaderFactory.CreateAutoShader(sourceFileName, "buffer_loads",
            DONUT_MAKE_PLATFORM_SHADER(g_gbuffer_vs_buffer_loads), &VertexShaderMacros, nvrhi::ShaderType::Vertex);
    }
}

template<GeometryInputPolicy InputPolicy>
nvrhi::ShaderHandle GBufferFillPassT<InputPolicy>::CreateGeometryShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{

    ShaderMacro MotionVectorsMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0");

    if (params.enableSinglePassCubemap)
    {
        // MVs will not work with cubemap views because:
        // 1. cubemap_gs does not pass through the previous position attribute;
        // 2. Computing correct MVs for a cubemap is complicated and not implemented.
        assert(!params.enableMotionVectors);

        auto desc = nvrhi::ShaderDesc()
            .setShaderType(nvrhi::ShaderType::Geometry)
            .setFastGSFlags(nvrhi::FastGeometryShaderFlags(
                nvrhi::FastGeometryShaderFlags::ForceFastGS |
                nvrhi::FastGeometryShaderFlags::UseViewportMask |
                nvrhi::FastGeometryShaderFlags::OffsetTargetIndexByViewportIndex))
            .setCoordinateSwizzling(CubemapView::GetCubemapCoordinateSwizzle());

        return shaderFactory.CreateAutoShader("donut/passes/cubemap_gs.hlsl", "main", DONUT_MAKE_PLATFORM_SHADER(g_cubemap_gs), nullptr, desc);
    }
    else
    {
        return nullptr;
    }
}

template<GeometryInputPolicy InputPolicy>
nvrhi::ShaderHandle GBufferFillPassT<InputPolicy>::CreatePixelShader(ShaderFactory& shaderFactory, const CreateParameters& params, bool alphaTested)
{
    std::vector<ShaderMacro> PixelShaderMacros;
    PixelShaderMacros.push_back(ShaderMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0"));
    PixelShaderMacros.push_back(ShaderMacro("ALPHA_TESTED", alphaTested ? "1" : "0"));

    return shaderFactory.CreateAutoShader("donut/passes/gbuffer_ps.hlsl", "main", DONUT_MAKE_PLATFORM_SHADER(g_gbuffer_ps), &PixelShaderMacros, nvrhi::ShaderType::Pixel);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::InputLayoutHandle GBufferFillPassT<InputPolicy>::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params)
{
    return CreateInputLayout(vertexShader, params, TexCoordFormat::Float32);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::InputLayoutHandle GBufferFillPassT<InputPolicy>::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params, TexCoordFormat texCoordFormat)
{
    if (params.useInputAssembler)
    {
        std::vector<nvrhi::VertexAttributeDesc> inputDescs =
        {
            GetVertexAttributeDesc(VertexAttribute::Position, "POS", 0),
            GetVertexAttributeDesc(VertexAttribute::PrevPosition, "PREV_POS", 1),
            GetVertexAttributeDesc(VertexAttribute::TexCoord1, "TEXCOORD", 2, texCoordFormat),
            GetVertexAttributeDesc(VertexAttribute::Normal, "NORMAL", 3),
            GetVertexAttributeDesc(VertexAttribute::Tangent, "TANGENT", 4),
            GetVertexAttributeDesc(VertexAttribute::Transform, "TRANSFORM", 5),
        };
        if (params.enableMotionVectors)
        {
            inputDescs.push_back(GetVertexAttributeDesc(VertexAttribute::PrevTransform, "PREV_TRANSFORM", 5));
        }

        return m_Device->createInputLayout(inputDescs.data(), static_cast<uint32_t>(inputDescs.size()), vertexShader);
    }

    return nullptr;
}

template<GeometryInputPolicy InputPolicy>
void GBufferFillPassT<InputPolicy>::CreateViewBindings(nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set, const CreateParameters& params)
{
    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .setRegisterSpaceAndDescriptorSet(GBUFFER_SPACE_VIEW)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(GBUFFER_BINDING_VIEW_CONSTANTS))
        .addItem(nvrhi::BindingLayoutItem::Sampler(GBUFFER_BINDING_MATERIAL_SAMPLER));

    layout = m_Device->createBindingLayout(bindingLayoutDesc);

    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .setTrackLiveness(params.trackLiveness)
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(GBUFFER_BINDING_VIEW_CONSTANTS, m_GBufferCB))
        .addItem(nvrhi::BindingSetItem::Sampler(GBUFFER_BINDING_MATERIAL_SAMPLER,
            m_CommonPasses->m_AnisotropicWrapSampler));

    set = m_Device->createBindingSet(bindingSetDesc, layout);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::GraphicsPipelineHandle GBufferFillPassT<InputPolicy>::CreateGraphicsPipeline(PipelineKey key, nvrhi::FramebufferInfo const& framebufferInfo)
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
    pipelineDesc.GS = m_GeometryShader;
    pipelineDesc.renderState.rasterState
        .setFrontCounterClockwise(key.bits.frontCounterClockwise)
        .setCullMode(key.bits.cullMode);
    pipelineDesc.renderState.blendState.disableAlphaToCoverage();
    pipelineDesc.bindingLayouts = { m_MaterialBindings->GetLayout(), m_ViewBindingLayout };
    pipelineDesc.bindingLayouts.push_back(floatingInput ? m_Input.floatBindingLayout : m_InputBindingLayout);

    pipelineDesc.renderState.depthStencilState
        .setDepthWriteEnable(m_EnableDepthWrite)
        .setDepthFunc(key.bits.reverseDepth
            ? nvrhi::ComparisonFunc::GreaterOrEqual
            : nvrhi::ComparisonFunc::LessOrEqual);
        
    if (m_StencilWriteMask)
    {
        pipelineDesc.renderState.depthStencilState
            .enableStencil()
            .setStencilReadMask(0)
            .setStencilWriteMask(uint8_t(m_StencilWriteMask))
            .setStencilRefValue(uint8_t(m_StencilWriteMask))
            .setFrontFaceStencil(nvrhi::DepthStencilState::StencilOpDesc().setPassOp(nvrhi::StencilOp::Replace))
            .setBackFaceStencil(nvrhi::DepthStencilState::StencilOpDesc().setPassOp(nvrhi::StencilOp::Replace));
    }

    if (key.bits.alphaTested)
    {
        pipelineDesc.renderState.rasterState.setCullNone();

        if (m_PixelShaderAlphaTested)
        {
            pipelineDesc.PS = m_PixelShaderAlphaTested;
        }
        else
        {
            pipelineDesc.PS = m_PixelShader;
            pipelineDesc.renderState.blendState.alphaToCoverageEnable = true;
        }
    }
    else
    {
        pipelineDesc.PS = m_PixelShader;
    }

    return m_Device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
}

template<GeometryInputPolicy InputPolicy>
std::shared_ptr<MaterialBindingCache> GBufferFillPassT<InputPolicy>::CreateMaterialBindingCache(CommonRenderPasses& commonPasses)
{
    std::vector<MaterialResourceBinding> materialBindings = {
        { MaterialResource::ConstantBuffer,         GBUFFER_BINDING_MATERIAL_CONSTANTS },
        { MaterialResource::DiffuseTexture,         GBUFFER_BINDING_MATERIAL_DIFFUSE_TEXTURE },
        { MaterialResource::SpecularTexture,        GBUFFER_BINDING_MATERIAL_SPECULAR_TEXTURE },
        { MaterialResource::NormalTexture,          GBUFFER_BINDING_MATERIAL_NORMAL_TEXTURE },
        { MaterialResource::EmissiveTexture,        GBUFFER_BINDING_MATERIAL_EMISSIVE_TEXTURE },
        { MaterialResource::OcclusionTexture,       GBUFFER_BINDING_MATERIAL_OCCLUSION_TEXTURE },
        { MaterialResource::TransmissionTexture,    GBUFFER_BINDING_MATERIAL_TRANSMISSION_TEXTURE },
        { MaterialResource::OpacityTexture,         GBUFFER_BINDING_MATERIAL_OPACITY_TEXTURE }
    };

    return std::make_shared<MaterialBindingCache>(
        m_Device,
        nvrhi::ShaderType::Pixel,
        /* registerSpace = */ GBUFFER_SPACE_MATERIAL,
        /* registerSpaceIsDescriptorSet = */ true,
        materialBindings,
        commonPasses.m_AnisotropicWrapSampler,
        commonPasses.m_GrayTexture,
        commonPasses.m_BlackTexture);
}

template<GeometryInputPolicy InputPolicy>
ViewType::Enum GBufferFillPassT<InputPolicy>::GetSupportedViewTypes() const
{
    return m_SupportedViewTypes;
}

template<GeometryInputPolicy InputPolicy>
void GBufferFillPassT<InputPolicy>::SetupView(GeometryPassContext& abstractContext, nvrhi::ICommandList* commandList, const engine::IView* view, const engine::IView* viewPrev)
{
    auto& context = static_cast<Context&>(abstractContext);
    context.pushConstantsValid = false;
    
    GBufferFillConstants gbufferConstants = {};
    view->FillPlanarViewConstants(gbufferConstants.view);
    viewPrev->FillPlanarViewConstants(gbufferConstants.viewPrev);
    commandList->writeBuffer(m_GBufferCB, &gbufferConstants, sizeof(gbufferConstants));

    context.keyTemplate.bits.frontCounterClockwise = view->IsMirrored();
    context.keyTemplate.bits.reverseDepth = view->IsReverseDepth();
}

template<GeometryInputPolicy InputPolicy>
bool GBufferFillPassT<InputPolicy>::SetupMaterial(GeometryPassContext& abstractContext, const engine::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state)
{
    auto& context = static_cast<Context&>(abstractContext);
    
    PipelineKey key = context.keyTemplate;
    key.bits.cullMode = cullMode;

    switch (material->domain)
    {
    case MaterialDomain::Opaque:
    case MaterialDomain::AlphaBlended: // Blended and transmissive domains are for the material ID pass, shouldn't be used otherwise
    case MaterialDomain::Transmissive:
    case MaterialDomain::TransmissiveAlphaTested:
    case MaterialDomain::TransmissiveAlphaBlended:
        key.bits.alphaTested = false;
        break;
    case MaterialDomain::AlphaTested:
        key.bits.alphaTested = true;
        break;
    default:
        return false;
    }

    nvrhi::IBindingSet* materialBindingSet = m_MaterialBindings->GetMaterialBindingSet(material);

    if (!materialBindingSet)
        return false;

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
    state.bindings = { materialBindingSet, m_ViewBindings };
    
    state.bindings.push_back(context.inputBindingSet);

    return true;
}

template<GeometryInputPolicy InputPolicy>
void GBufferFillPassT<InputPolicy>::SetupInputBuffers(GeometryPassContext& abstractContext, const engine::BufferGroup* buffers, nvrhi::GraphicsState& state)
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
            { buffers->vertexBuffer, 1, buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset },
            { buffers->vertexBuffer, 2, buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset },
            { buffers->vertexBuffer, 3, buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset },
            { buffers->vertexBuffer, 4, buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset },
            { buffers->instanceBuffer, 5, 0 }
        };
    }
    else
    {
        context.positionOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset);
        context.prevPositionOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset);
        context.texCoordOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        context.normalOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset);
        context.tangentOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset);
    }
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingLayoutHandle GBufferFillPassT<InputPolicy>::CreateInputBindingLayout()
{
    return GeometryPassInput<InputPolicy>::template CreateBindingLayout<GBufferInputConfiguration>(
        m_Device, m_UseInputAssembler, m_IsDX11);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingSetHandle GBufferFillPassT<InputPolicy>::CreateInputBindingSet(const BufferGroup* bufferGroup)
{
    return GeometryPassInput<InputPolicy>::template CreateBindingSet<GBufferInputConfiguration>(
        m_Device, m_InputBindingLayout, bufferGroup, m_UseInputAssembler, m_IsDX11);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingSetHandle GBufferFillPassT<InputPolicy>::GetOrCreateInputBindingSet(const BufferGroup* bufferGroup)
{
    return m_Input.GetBindingSet(bufferGroup, [&](const BufferGroup* buffers)
    {
        if constexpr (InputPolicy == GeometryInputPolicy::Stock)
            return GeometryPassInput<InputPolicy>::template CreateBindingSet<GBufferInputConfiguration>(
                m_Device, m_InputBindingLayout, buffers, m_UseInputAssembler, m_IsDX11);
        else
            return CreateInputBindingSet(buffers);
    });
}

template<GeometryInputPolicy InputPolicy>
void GBufferFillPassT<InputPolicy>::SetPushConstants(
    donut::render::GeometryPassContext& abstractContext,
    nvrhi::ICommandList* commandList,
    nvrhi::GraphicsState& state,
    nvrhi::DrawArguments& args)
{
    auto& context = static_cast<Context&>(abstractContext);
    m_Input.template SetPushConstants<GBufferInputConfiguration>(context, commandList, args, m_UseInputAssembler);
}

void MaterialIDPass::Init(
    engine::ShaderFactory& shaderFactory,
    const CreateParameters& params)
{
    CreateParameters paramsCopy = params;
    // The material ID pass relies on the push constants filled by the buffer load path (firstInstance)
    paramsCopy.useInputAssembler = false;
    // The material ID pass doesn't support generating motion vectors
    paramsCopy.enableMotionVectors = false;

    GBufferFillPass::Init(shaderFactory, paramsCopy);
}

nvrhi::ShaderHandle MaterialIDPass::CreatePixelShader(engine::ShaderFactory& shaderFactory, const CreateParameters& params, bool alphaTested)
{
    std::vector<ShaderMacro> PixelShaderMacros;
    PixelShaderMacros.push_back(ShaderMacro("ALPHA_TESTED", alphaTested ? "1" : "0"));

    return shaderFactory.CreateAutoShader("donut/passes/material_id_ps.hlsl", "main",
        DONUT_MAKE_PLATFORM_SHADER(g_material_id_ps), &PixelShaderMacros, nvrhi::ShaderType::Pixel);
}

template class donut::render::detail::GBufferFillPassT<GeometryInputPolicy::Stock>;
template class donut::render::detail::GBufferFillPassT<GeometryInputPolicy::Custom>;
