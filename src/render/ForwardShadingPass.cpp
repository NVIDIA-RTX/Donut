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

#include <donut/render/ForwardShadingPass.h>
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
#include "compiled_shaders/passes/forward_ps.dxbc.h"
#include "compiled_shaders/passes/forward_vs_input_assembler.dxbc.h"
#include "compiled_shaders/passes/forward_vs_buffer_loads.dxbc.h"
#endif
#if DONUT_WITH_DX12
#include "compiled_shaders/passes/cubemap_gs.dxil.h"
#include "compiled_shaders/passes/forward_ps.dxil.h"
#include "compiled_shaders/passes/forward_vs_input_assembler.dxil.h"
#include "compiled_shaders/passes/forward_vs_buffer_loads.dxil.h"
#endif
#if DONUT_WITH_VULKAN
#include "compiled_shaders/passes/cubemap_gs.spirv.h"
#include "compiled_shaders/passes/forward_ps.spirv.h"
#include "compiled_shaders/passes/forward_vs_input_assembler.spirv.h"
#include "compiled_shaders/passes/forward_vs_buffer_loads.spirv.h"
#endif
#endif

using namespace donut::math;
#include <donut/shaders/forward_cb.h>


using namespace donut::engine;
using namespace donut::render;
using namespace donut::render::detail;

namespace
{
    struct ForwardInputConfiguration
    {
        using PushConstants = ForwardPushConstants;
        static constexpr uint32_t InputSpace = FORWARD_SPACE_INPUT;
        static constexpr uint32_t PushConstantBinding = FORWARD_BINDING_PUSH_CONSTANTS;
        static constexpr uint32_t InstanceBinding = FORWARD_BINDING_INSTANCE_BUFFER;
        static constexpr uint32_t VertexBinding = FORWARD_BINDING_VERTEX_BUFFER;
        static constexpr nvrhi::ShaderType Visibility = nvrhi::ShaderType::Vertex;
        static constexpr bool HasNormals = true;
        static constexpr bool HasPrevPosition = false;
    };
}

template<GeometryInputPolicy InputPolicy>
ForwardShadingPassImpl<InputPolicy>::ForwardShadingPassImpl(
    nvrhi::IDevice* device,
    std::shared_ptr<CommonRenderPasses> commonPasses)
    : m_Device(device)
    , m_CommonPasses(std::move(commonPasses))
{
    m_IsDX11 = m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
}

template<GeometryInputPolicy InputPolicy>
void ForwardShadingPassImpl<InputPolicy>::Init(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    m_UseInputAssembler = params.useInputAssembler;

    m_SupportedViewTypes = ViewType::PLANAR;
    if (params.singlePassCubemap)
        m_SupportedViewTypes = ViewType::CUBEMAP;
    
    m_Input.template Init<ForwardInputConfiguration>(m_UseInputAssembler, [&]()
    {
        std::vector<ShaderMacro> macros = { { "DECODE_TEXCOORD", "0" } };
        return shaderFactory.CreateAutoShader("donut/passes/forward_vs.hlsl", "input_assembler",
            DONUT_MAKE_PLATFORM_SHADER(g_forward_vs_input_assembler), &macros, nvrhi::ShaderType::Vertex);
    });

    m_VertexShader = CreateVertexShader(shaderFactory, params);
    m_InputLayouts = {};
    m_InputLayouts[size_t(TexCoordFormat::Float32)] = CreateInputLayout(
        m_Input.UsesSpecializedInput() ? m_Input.floatVertexShader : m_VertexShader, params);
    m_CreateParameters = params;
    m_GeometryShader = CreateGeometryShader(shaderFactory, params);
    m_PixelShader = CreatePixelShader(shaderFactory, params, false);
    m_PixelShaderTransmissive = CreatePixelShader(shaderFactory, params, true);

    if (params.materialBindings)
        m_MaterialBindings = params.materialBindings;
    else
        m_MaterialBindings = CreateMaterialBindingCache(*m_CommonPasses);

    auto samplerDesc = nvrhi::SamplerDesc()
        .setAllAddressModes(nvrhi::SamplerAddressMode::Border)
        .setBorderColor(1.0f);
    m_ShadowSampler = m_Device->createSampler(samplerDesc);

    m_ForwardViewCB = m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ForwardShadingViewConstants), "ForwardShadingViewConstants", params.numConstantBufferVersions));
    m_ForwardLightCB = m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ForwardShadingLightConstants), "ForwardShadingLightConstants", params.numConstantBufferVersions));

    m_ViewBindingLayout = CreateViewBindingLayout();
    m_ViewBindingSet = CreateViewBindingSet();
    m_ShadingBindingLayout = CreateShadingBindingLayout();
    if constexpr (InputPolicy == GeometryInputPolicy::Stock)
        m_InputBindingLayout = GeometryPassInput<InputPolicy>::template CreateBindingLayout<ForwardInputConfiguration>(
            m_Device, m_UseInputAssembler, m_IsDX11);
    else
        m_InputBindingLayout = CreateInputBindingLayout();
    m_Input.template InitBindingSet<ForwardInputConfiguration>(m_Device, m_InputBindingLayout);
}

template<GeometryInputPolicy InputPolicy>
void ForwardShadingPassImpl<InputPolicy>::ResetBindingCache()
{
    m_MaterialBindings->Clear();
    m_ShadingBindingSets.clear();
    m_Input.ResetBindingCache();
}

template<GeometryInputPolicy InputPolicy>
nvrhi::ShaderHandle ForwardShadingPassImpl<InputPolicy>::CreateVertexShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    char const* sourceFileName = "donut/passes/forward_vs.hlsl";

    if (params.useInputAssembler)
    {
        std::vector<ShaderMacro> macros = { { "DECODE_TEXCOORD", "1" } };
        return shaderFactory.CreateAutoShader(sourceFileName, "input_assembler",
            DONUT_MAKE_PLATFORM_SHADER(g_forward_vs_input_assembler), &macros, nvrhi::ShaderType::Vertex);
    }
    else
    {
        return shaderFactory.CreateAutoShader(sourceFileName, "buffer_loads",
            DONUT_MAKE_PLATFORM_SHADER(g_forward_vs_buffer_loads), nullptr, nvrhi::ShaderType::Vertex);
    }
}

template<GeometryInputPolicy InputPolicy>
nvrhi::ShaderHandle ForwardShadingPassImpl<InputPolicy>::CreateGeometryShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    if (params.singlePassCubemap)
    {
        auto desc = nvrhi::ShaderDesc()
            .setShaderType(nvrhi::ShaderType::Geometry)
            .setFastGSFlags(nvrhi::FastGeometryShaderFlags(
                nvrhi::FastGeometryShaderFlags::ForceFastGS |
                nvrhi::FastGeometryShaderFlags::UseViewportMask |
                nvrhi::FastGeometryShaderFlags::OffsetTargetIndexByViewportIndex))
            .setCoordinateSwizzling(CubemapView::GetCubemapCoordinateSwizzle());

        return shaderFactory.CreateAutoShader("donut/passes/cubemap_gs.hlsl", "main", DONUT_MAKE_PLATFORM_SHADER(g_cubemap_gs), nullptr, desc);
    }

    return nullptr;
}

template<GeometryInputPolicy InputPolicy>
nvrhi::ShaderHandle ForwardShadingPassImpl<InputPolicy>::CreatePixelShader(ShaderFactory& shaderFactory, const CreateParameters& params, bool transmissiveMaterial)
{
    std::vector<ShaderMacro> Macros;
    Macros.push_back(ShaderMacro("TRANSMISSIVE_MATERIAL", transmissiveMaterial ? "1" : "0"));

    return shaderFactory.CreateAutoShader("donut/passes/forward_ps.hlsl", "main", DONUT_MAKE_PLATFORM_SHADER(g_forward_ps), &Macros, nvrhi::ShaderType::Pixel);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::InputLayoutHandle ForwardShadingPassImpl<InputPolicy>::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params)
{
    return CreateInputLayout(vertexShader, params, TexCoordFormat::Float32);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::InputLayoutHandle ForwardShadingPassImpl<InputPolicy>::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params, TexCoordFormat texCoordFormat)
{
    if (params.useInputAssembler)
    {
        const nvrhi::VertexAttributeDesc inputDescs[] =
        {
            GetVertexAttributeDesc(VertexAttribute::Position, "POS", 0),
            GetVertexAttributeDesc(VertexAttribute::PrevPosition, "PREV_POS", 1),
            GetVertexAttributeDesc(VertexAttribute::TexCoord1, "TEXCOORD", 2, texCoordFormat),
            GetVertexAttributeDesc(VertexAttribute::Normal, "NORMAL", 3),
            GetVertexAttributeDesc(VertexAttribute::Tangent, "TANGENT", 4),
            GetVertexAttributeDesc(VertexAttribute::Transform, "TRANSFORM", 5),
        };

        return m_Device->createInputLayout(inputDescs, uint32_t(std::size(inputDescs)), vertexShader);
    }
    
    return nullptr;
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingLayoutHandle ForwardShadingPassImpl<InputPolicy>::CreateViewBindingLayout()
{
    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_VIEW)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(FORWARD_BINDING_VIEW_CONSTANTS));

    return m_Device->createBindingLayout(bindingLayoutDesc);
}


template<GeometryInputPolicy InputPolicy>
nvrhi::BindingSetHandle ForwardShadingPassImpl<InputPolicy>::CreateViewBindingSet()
{
    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .setTrackLiveness(m_TrackLiveness)
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(FORWARD_BINDING_VIEW_CONSTANTS, m_ForwardViewCB));

    return m_Device->createBindingSet(bindingSetDesc, m_ViewBindingLayout);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingLayoutHandle ForwardShadingPassImpl<InputPolicy>::CreateShadingBindingLayout()
{
    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Pixel)
        .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_SHADING)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(FORWARD_BINDING_LIGHT_CONSTANTS))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(FORWARD_BINDING_SHADOW_MAP_TEXTURE))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(FORWARD_BINDING_DIFFUSE_LIGHT_PROBE_TEXTURE))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(FORWARD_BINDING_SPECULAR_LIGHT_PROBE_TEXTURE))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(FORWARD_BINDING_ENVIRONMENT_BRDF_TEXTURE))
        .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_MATERIAL_SAMPLER))
        .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_SHADOW_MAP_SAMPLER))
        .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_LIGHT_PROBE_SAMPLER))
        .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_ENVIRONMENT_BRDF_SAMPLER));

    return m_Device->createBindingLayout(bindingLayoutDesc);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingSetHandle ForwardShadingPassImpl<InputPolicy>::CreateShadingBindingSet(nvrhi::ITexture* shadowMapTexture,
    nvrhi::ITexture* diffuse, nvrhi::ITexture* specular, nvrhi::ITexture* environmentBrdf)
{
    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .setTrackLiveness(m_TrackLiveness)
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(FORWARD_BINDING_LIGHT_CONSTANTS, m_ForwardLightCB))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(FORWARD_BINDING_SHADOW_MAP_TEXTURE,
            shadowMapTexture ? shadowMapTexture : m_CommonPasses->m_BlackTexture2DArray.Get()))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(FORWARD_BINDING_DIFFUSE_LIGHT_PROBE_TEXTURE,
            diffuse ? diffuse : m_CommonPasses->m_BlackCubeMapArray.Get()))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(FORWARD_BINDING_SPECULAR_LIGHT_PROBE_TEXTURE,
            specular ? specular : m_CommonPasses->m_BlackCubeMapArray.Get()))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(FORWARD_BINDING_ENVIRONMENT_BRDF_TEXTURE,
            environmentBrdf ? environmentBrdf : m_CommonPasses->m_BlackTexture.Get()))
        .addItem(nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_MATERIAL_SAMPLER,
            m_CommonPasses->m_AnisotropicWrapSampler))
        .addItem(nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_SHADOW_MAP_SAMPLER,
            m_ShadowSampler))
        .addItem(nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_LIGHT_PROBE_SAMPLER,
            m_CommonPasses->m_LinearWrapSampler))
        .addItem(nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_ENVIRONMENT_BRDF_SAMPLER,
            m_CommonPasses->m_LinearClampSampler));

    return m_Device->createBindingSet(bindingSetDesc, m_ShadingBindingLayout);
}


template<GeometryInputPolicy InputPolicy>
nvrhi::GraphicsPipelineHandle ForwardShadingPassImpl<InputPolicy>::CreateGraphicsPipeline(ForwardShadingPassPipelineKey const& key,
    nvrhi::FramebufferInfo const& framebufferInfo)
{
    const bool floatingInput = m_Input.UsesSpecializedInput() && key.texCoordFormat != TexCoordFormat::Unorm16;
    const auto& vertexShader = floatingInput ? m_Input.floatVertexShader : m_VertexShader;
    if (size_t(key.texCoordFormat) >= m_InputLayouts.size())
        return nullptr;
    auto& inputLayout = m_InputLayouts[size_t(key.texCoordFormat)];

    if (key.texCoordFormat != TexCoordFormat::Float32 && !inputLayout)
    {
        inputLayout = CreateInputLayout(vertexShader, m_CreateParameters, key.texCoordFormat);
        if (!inputLayout)
            return nullptr;
    }

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.inputLayout = inputLayout;
    pipelineDesc.VS = vertexShader;
    pipelineDesc.GS = m_GeometryShader;
    pipelineDesc.renderState.rasterState.frontCounterClockwise = key.frontCounterClockwise;
    pipelineDesc.renderState.rasterState.setCullMode(key.cullMode);
    pipelineDesc.renderState.blendState.alphaToCoverageEnable = false;
    pipelineDesc.shadingRateState = key.shadingRateState;
    pipelineDesc.bindingLayouts = { m_MaterialBindings->GetLayout(), m_ViewBindingLayout, m_ShadingBindingLayout };
    pipelineDesc.bindingLayouts.push_back(floatingInput ? m_Input.floatBindingLayout : m_InputBindingLayout);

    bool const framebufferUsesMSAA = framebufferInfo.sampleCount > 1;

    pipelineDesc.renderState.depthStencilState
        .setDepthFunc(key.reverseDepth
            ? nvrhi::ComparisonFunc::GreaterOrEqual
            : nvrhi::ComparisonFunc::LessOrEqual);
    
    switch (key.domain)
    {
    case MaterialDomain::Opaque:
        pipelineDesc.PS = m_PixelShader;
        break;

    case MaterialDomain::AlphaTested:
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.renderState.blendState.alphaToCoverageEnable = framebufferUsesMSAA;
        break;

    case MaterialDomain::AlphaBlended: {
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.renderState.blendState.targets[0]
            .enableBlend()
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
            .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
            .setDestBlendAlpha(nvrhi::BlendFactor::One);
        
        pipelineDesc.renderState.depthStencilState.disableDepthWrite();
        break;
    }

    case MaterialDomain::Transmissive:
    case MaterialDomain::TransmissiveAlphaTested:
    case MaterialDomain::TransmissiveAlphaBlended: {
        pipelineDesc.PS = m_PixelShaderTransmissive;
        pipelineDesc.renderState.blendState.targets[0]
            .enableBlend()
            .setSrcBlend(nvrhi::BlendFactor::One)
            .setDestBlend(nvrhi::BlendFactor::Src1Color)
            .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
            .setDestBlendAlpha(nvrhi::BlendFactor::One);

        pipelineDesc.renderState.depthStencilState.disableDepthWrite();
        break;
    }
    default:
        return nullptr;
    }

    return m_Device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
}

template<GeometryInputPolicy InputPolicy>
std::shared_ptr<MaterialBindingCache> ForwardShadingPassImpl<InputPolicy>::CreateMaterialBindingCache(CommonRenderPasses& commonPasses)
{
    std::vector<MaterialResourceBinding> materialBindings = {
        { MaterialResource::ConstantBuffer,         FORWARD_BINDING_MATERIAL_CONSTANTS },
        { MaterialResource::DiffuseTexture,         FORWARD_BINDING_MATERIAL_DIFFUSE_TEXTURE },
        { MaterialResource::SpecularTexture,        FORWARD_BINDING_MATERIAL_SPECULAR_TEXTURE },
        { MaterialResource::NormalTexture,          FORWARD_BINDING_MATERIAL_NORMAL_TEXTURE },
        { MaterialResource::EmissiveTexture,        FORWARD_BINDING_MATERIAL_EMISSIVE_TEXTURE },
        { MaterialResource::OcclusionTexture,       FORWARD_BINDING_MATERIAL_OCCLUSION_TEXTURE },
        { MaterialResource::TransmissionTexture,    FORWARD_BINDING_MATERIAL_TRANSMISSION_TEXTURE },
        { MaterialResource::OpacityTexture,         FORWARD_BINDING_MATERIAL_OPACITY_TEXTURE }
    };

    return std::make_shared<MaterialBindingCache>(
        m_Device,
        nvrhi::ShaderType::Pixel,
        /* registerSpace = */ FORWARD_SPACE_MATERIAL,
        /* registerSpaceIsDescriptorSet = */ true,
        materialBindings,
        commonPasses.m_AnisotropicWrapSampler,
        commonPasses.m_GrayTexture,
        commonPasses.m_BlackTexture);
}

template<GeometryInputPolicy InputPolicy>
void ForwardShadingPassImpl<InputPolicy>::SetupView(
    GeometryPassContext& abstractContext,
    nvrhi::ICommandList* commandList,
    const IView* view,
    const IView* viewPrev)
{
    auto& context = static_cast<Context&>(abstractContext);
    context.pushConstantsValid = false;
    
    ForwardShadingViewConstants viewConstants = {};
    view->FillPlanarViewConstants(viewConstants.view);
    commandList->writeBuffer(m_ForwardViewCB, &viewConstants, sizeof(viewConstants));

    context.keyTemplate.frontCounterClockwise = view->IsMirrored();
    context.keyTemplate.reverseDepth = view->IsReverseDepth();
    context.keyTemplate.shadingRateState = view->GetVariableRateShadingState();
}

template<GeometryInputPolicy InputPolicy>
void ForwardShadingPassImpl<InputPolicy>::PrepareLights(
    Context& context,
    nvrhi::ICommandList* commandList,
    const std::vector<std::shared_ptr<Light>>& lights,
    dm::float3 ambientColorTop,
    dm::float3 ambientColorBottom,
    const std::vector<std::shared_ptr<LightProbe>>& lightProbes)
{
    nvrhi::ITexture* shadowMapTexture = nullptr;
    int2 shadowMapTextureSize = 0;
    for (const auto& light : lights)
    {
        if (light->shadowMap)
        {
            shadowMapTexture = light->shadowMap->GetTexture();
            shadowMapTextureSize = light->shadowMap->GetTextureSize();
            break;
        }
    }

    nvrhi::ITexture* lightProbeDiffuse = nullptr;
    nvrhi::ITexture* lightProbeSpecular = nullptr;
    nvrhi::ITexture* lightProbeEnvironmentBrdf = nullptr;

    for (const auto& probe : lightProbes)
    {
        if (!probe->enabled)
            continue;

        if (lightProbeDiffuse == nullptr || lightProbeSpecular == nullptr || lightProbeEnvironmentBrdf == nullptr)
        {
            lightProbeDiffuse = probe->diffuseMap;
            lightProbeSpecular = probe->specularMap;
            lightProbeEnvironmentBrdf = probe->environmentBrdf;
        }
        else
        {
            if (lightProbeDiffuse != probe->diffuseMap || lightProbeSpecular != probe->specularMap || lightProbeEnvironmentBrdf != probe->environmentBrdf)
            {
                log::error("All lights probe submitted to ForwardShadingPass::PrepareLights(...) must use the same set of textures");
                return;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lockGuard(m_Mutex);

        nvrhi::BindingSetHandle& shadingBindings = m_ShadingBindingSets[std::make_pair(shadowMapTexture, lightProbeDiffuse)];

        if (!shadingBindings)
        {
            shadingBindings = CreateShadingBindingSet(shadowMapTexture, lightProbeDiffuse, lightProbeSpecular, lightProbeEnvironmentBrdf);
        }

        context.shadingBindingSet = shadingBindings;
    }


    ForwardShadingLightConstants constants = {};

    constants.shadowMapTextureSize = float2(shadowMapTextureSize);
    constants.shadowMapTextureSizeInv = 1.f / constants.shadowMapTextureSize;

    int numShadows = 0;

    for (int nLight = 0; nLight < std::min(static_cast<int>(lights.size()), FORWARD_MAX_LIGHTS); nLight++)
    {
        const auto& light = lights[nLight];

        LightConstants& lightConstants = constants.lights[constants.numLights];
        light->FillLightConstants(lightConstants);

        if (light->shadowMap)
        {
            for (uint32_t cascade = 0; cascade < light->shadowMap->GetNumberOfCascades(); cascade++)
            {
                if (numShadows < FORWARD_MAX_SHADOWS)
                {
                    light->shadowMap->GetCascade(cascade)->FillShadowConstants(constants.shadows[numShadows]);
                    lightConstants.shadowCascades[cascade] = numShadows;
                    ++numShadows;
                }
            }

            for (uint32_t perObjectShadow = 0; perObjectShadow < light->shadowMap->GetNumberOfPerObjectShadows(); perObjectShadow++)
            {
                if (numShadows < FORWARD_MAX_SHADOWS)
                {
                    light->shadowMap->GetPerObjectShadow(perObjectShadow)->FillShadowConstants(constants.shadows[numShadows]);
                    lightConstants.perObjectShadows[perObjectShadow] = numShadows;
                    ++numShadows;
                }
            }
        }

        ++constants.numLights;
    }

    constants.ambientColorTop = float4(ambientColorTop, 0.f);
    constants.ambientColorBottom = float4(ambientColorBottom, 0.f);

    for (const auto& probe : lightProbes)
    {
        if (!probe->IsActive())
            continue;

        LightProbeConstants& lightProbeConstants = constants.lightProbes[constants.numLightProbes];
        probe->FillLightProbeConstants(lightProbeConstants);

        ++constants.numLightProbes;

        if (constants.numLightProbes >= FORWARD_MAX_LIGHT_PROBES)
            break;
    }

    commandList->writeBuffer(m_ForwardLightCB, &constants, sizeof(constants));
}

template<GeometryInputPolicy InputPolicy>
ViewType::Enum ForwardShadingPassImpl<InputPolicy>::GetSupportedViewTypes() const
{
    return m_SupportedViewTypes;
}

template<GeometryInputPolicy InputPolicy>
bool ForwardShadingPassImpl<InputPolicy>::SetupMaterial(GeometryPassContext& abstractContext, const Material* material,
    nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state)
{
    auto& context = static_cast<Context&>(abstractContext);

    nvrhi::IBindingSet* materialBindingSet = m_MaterialBindings->GetMaterialBindingSet(material);

    if (!materialBindingSet)
        return false;

    if (material->domain >= MaterialDomain::Count || cullMode > nvrhi::RasterCullMode::None)
    {
        assert(false);
        return false;
    }

    ForwardShadingPassPipelineKey key = context.keyTemplate;
    key.cullMode = cullMode;
    key.domain = material->domain;

    nvrhi::GraphicsPipelineHandle& pipeline = m_Pipelines[key];

    if (!pipeline)
    {
        std::lock_guard<std::mutex> lockGuard(m_Mutex);

        if (!pipeline)
            pipeline = CreateGraphicsPipeline(key, state.framebuffer->getFramebufferInfo());

        if (!pipeline)
            return false;
    }

    assert(pipeline->getFramebufferInfo() == state.framebuffer->getFramebufferInfo());

    state.pipeline = pipeline;
    state.bindings = { materialBindingSet, m_ViewBindingSet, context.shadingBindingSet };
    
    state.bindings.push_back(context.inputBindingSet);

    return true;
}

template<GeometryInputPolicy InputPolicy>
void ForwardShadingPassImpl<InputPolicy>::SetupInputBuffers(GeometryPassContext& abstractContext, const BufferGroup* buffers, nvrhi::GraphicsState& state)
{
    auto& context = static_cast<Context&>(abstractContext);

    context.inputBuffers = buffers;
    context.inputBindingSet = GetOrCreateInputBindingSet(buffers);
    context.texCoordFormat = buffers->texCoordFormat;
    context.keyTemplate.texCoordFormat = m_UseInputAssembler ? buffers->texCoordFormat : TexCoordFormat::Float32;
    
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
        context.texCoordOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        context.normalOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset);
        context.tangentOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset);
    }
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingLayoutHandle ForwardShadingPassImpl<InputPolicy>::CreateInputBindingLayout()
{
    return GeometryPassInput<InputPolicy>::template CreateBindingLayout<ForwardInputConfiguration>(
        m_Device, m_UseInputAssembler, m_IsDX11);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingSetHandle ForwardShadingPassImpl<InputPolicy>::CreateInputBindingSet(const BufferGroup* bufferGroup)
{
    return GeometryPassInput<InputPolicy>::template CreateBindingSet<ForwardInputConfiguration>(
        m_Device, m_InputBindingLayout, bufferGroup, m_UseInputAssembler, m_IsDX11);
}

template<GeometryInputPolicy InputPolicy>
nvrhi::BindingSetHandle ForwardShadingPassImpl<InputPolicy>::GetOrCreateInputBindingSet(const BufferGroup* bufferGroup)
{
    return m_Input.GetBindingSet(bufferGroup, [&](const BufferGroup* buffers)
    {
        if constexpr (InputPolicy == GeometryInputPolicy::Stock)
            return GeometryPassInput<InputPolicy>::template CreateBindingSet<ForwardInputConfiguration>(
                m_Device, m_InputBindingLayout, buffers, m_UseInputAssembler, m_IsDX11);
        else
            return CreateInputBindingSet(buffers);
    });
}

template<GeometryInputPolicy InputPolicy>
void ForwardShadingPassImpl<InputPolicy>::SetPushConstants(
    donut::render::GeometryPassContext& abstractContext,
    nvrhi::ICommandList* commandList,
    nvrhi::GraphicsState& state,
    nvrhi::DrawArguments& args)
{
    auto& context = static_cast<Context&>(abstractContext);
    m_Input.template SetPushConstants<ForwardInputConfiguration>(context, commandList, args, m_UseInputAssembler);
}

template class donut::render::detail::ForwardShadingPassImpl<GeometryInputPolicy::Stock>;
template class donut::render::detail::ForwardShadingPassImpl<GeometryInputPolicy::Custom>;
