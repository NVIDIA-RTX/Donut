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
#include <cstring>
#include <typeinfo>

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

ForwardShadingPass::ForwardShadingPass(
    nvrhi::IDevice* device,
    std::shared_ptr<CommonRenderPasses> commonPasses)
    : m_Device(device)
    , m_CommonPasses(std::move(commonPasses))
{
    m_IsDX11 = m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
}

void ForwardShadingPass::Init(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    m_UseInputAssembler = params.useInputAssembler;

    m_SupportedViewTypes = ViewType::PLANAR;
    if (params.singlePassCubemap)
        m_SupportedViewTypes = ViewType::CUBEMAP;
    
    m_SpecializeInputAssemblerTexCoords = m_UseInputAssembler
        && params.specializeInputAssemblerTexCoords.value_or(typeid(*this) == typeid(ForwardShadingPass));
    m_VertexShaderFloat = nullptr;
    m_InputBindingLayoutFloat = nullptr;
    m_InputBindingSetFloat = nullptr;
    m_InputBindingSetUnorm = nullptr;
    if (m_SpecializeInputAssemblerTexCoords)
    {
        std::vector<ShaderMacro> macros = { { "DECODE_TEXCOORD", "0" } };
        m_VertexShaderFloat = shaderFactory.CreateAutoShader("donut/passes/forward_vs.hlsl", "input_assembler",
            DONUT_MAKE_PLATFORM_SHADER(g_forward_vs_input_assembler), &macros, nvrhi::ShaderType::Vertex);
        if (!m_VertexShaderFloat)
            m_SpecializeInputAssemblerTexCoords = false;
    }

    m_VertexShader = CreateVertexShader(shaderFactory, params);
    m_InputLayouts = {};
    m_InputLayouts[size_t(TexCoordFormat::Float32)] = CreateInputLayout(
        m_SpecializeInputAssemblerTexCoords ? m_VertexShaderFloat : m_VertexShader, params);
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
    m_InputBindingLayout = CreateInputBindingLayout();
    if (m_SpecializeInputAssemblerTexCoords)
    {
        m_InputBindingLayoutFloat = m_Device->createBindingLayout(nvrhi::BindingLayoutDesc()
            .setVisibility(nvrhi::ShaderType::Vertex)
            .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_INPUT));
        m_InputBindingSetFloat = m_Device->createBindingSet(nvrhi::BindingSetDesc(), m_InputBindingLayoutFloat);
        // The stock IA binding set contains only push constants, so it needs no BufferGroup.
        m_InputBindingSetUnorm = CreateInputBindingSet(nullptr);
    }
}

void ForwardShadingPass::ResetBindingCache()
{
    m_MaterialBindings->Clear();
    m_ShadingBindingSets.clear();
    m_InputBindingSets.clear();
}

nvrhi::ShaderHandle ForwardShadingPass::CreateVertexShader(ShaderFactory& shaderFactory, const CreateParameters& params)
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

nvrhi::ShaderHandle ForwardShadingPass::CreateGeometryShader(ShaderFactory& shaderFactory, const CreateParameters& params)
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

nvrhi::ShaderHandle ForwardShadingPass::CreatePixelShader(ShaderFactory& shaderFactory, const CreateParameters& params, bool transmissiveMaterial)
{
    std::vector<ShaderMacro> Macros;
    Macros.push_back(ShaderMacro("TRANSMISSIVE_MATERIAL", transmissiveMaterial ? "1" : "0"));

    return shaderFactory.CreateAutoShader("donut/passes/forward_ps.hlsl", "main", DONUT_MAKE_PLATFORM_SHADER(g_forward_ps), &Macros, nvrhi::ShaderType::Pixel);
}

nvrhi::InputLayoutHandle ForwardShadingPass::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params)
{
    return CreateInputLayout(vertexShader, params, TexCoordFormat::Float32);
}

nvrhi::InputLayoutHandle ForwardShadingPass::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params, TexCoordFormat texCoordFormat)
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

nvrhi::BindingLayoutHandle ForwardShadingPass::CreateViewBindingLayout()
{
    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_VIEW)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(FORWARD_BINDING_VIEW_CONSTANTS));

    return m_Device->createBindingLayout(bindingLayoutDesc);
}


nvrhi::BindingSetHandle ForwardShadingPass::CreateViewBindingSet()
{
    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .setTrackLiveness(m_TrackLiveness)
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(FORWARD_BINDING_VIEW_CONSTANTS, m_ForwardViewCB));

    return m_Device->createBindingSet(bindingSetDesc, m_ViewBindingLayout);
}

nvrhi::BindingLayoutHandle ForwardShadingPass::CreateShadingBindingLayout()
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

nvrhi::BindingSetHandle ForwardShadingPass::CreateShadingBindingSet(nvrhi::ITexture* shadowMapTexture,
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


nvrhi::GraphicsPipelineHandle ForwardShadingPass::CreateGraphicsPipeline(ForwardShadingPassPipelineKey const& key,
    nvrhi::FramebufferInfo const& framebufferInfo)
{
    const bool floatingInput = m_SpecializeInputAssemblerTexCoords && key.texCoordFormat != TexCoordFormat::Unorm16;
    const auto& vertexShader = floatingInput ? m_VertexShaderFloat : m_VertexShader;
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
    pipelineDesc.bindingLayouts.push_back(floatingInput ? m_InputBindingLayoutFloat : m_InputBindingLayout);

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

std::shared_ptr<MaterialBindingCache> ForwardShadingPass::CreateMaterialBindingCache(CommonRenderPasses& commonPasses)
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

void ForwardShadingPass::SetupView(
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

void ForwardShadingPass::PrepareLights(
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

ViewType::Enum ForwardShadingPass::GetSupportedViewTypes() const
{
    return m_SupportedViewTypes;
}

bool ForwardShadingPass::SetupMaterial(GeometryPassContext& abstractContext, const Material* material,
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

void ForwardShadingPass::SetupInputBuffers(GeometryPassContext& abstractContext, const BufferGroup* buffers, nvrhi::GraphicsState& state)
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

nvrhi::BindingLayoutHandle ForwardShadingPass::CreateInputBindingLayout()
{
    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex)
        .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_INPUT)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(FORWARD_BINDING_PUSH_CONSTANTS, sizeof(ForwardPushConstants)));

    if (!m_UseInputAssembler)
    {
        bindingLayoutDesc
            .addItem(m_IsDX11
                ? nvrhi::BindingLayoutItem::RawBuffer_SRV(FORWARD_BINDING_INSTANCE_BUFFER)
                : nvrhi::BindingLayoutItem::StructuredBuffer_SRV(FORWARD_BINDING_INSTANCE_BUFFER))
            .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(FORWARD_BINDING_VERTEX_BUFFER));
    }

    return m_Device->createBindingLayout(bindingLayoutDesc);
}

nvrhi::BindingSetHandle ForwardShadingPass::CreateInputBindingSet(const BufferGroup* bufferGroup)
{
    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::PushConstants(FORWARD_BINDING_PUSH_CONSTANTS, sizeof(ForwardPushConstants)));

    if (!m_UseInputAssembler)
    {
        bindingSetDesc
            .addItem(m_IsDX11
                ? nvrhi::BindingSetItem::RawBuffer_SRV(FORWARD_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer)
                : nvrhi::BindingSetItem::StructuredBuffer_SRV(FORWARD_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer))
            .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(FORWARD_BINDING_VERTEX_BUFFER, bufferGroup->vertexBuffer));
    }

    return m_Device->createBindingSet(bindingSetDesc, m_InputBindingLayout);
}

nvrhi::BindingSetHandle ForwardShadingPass::GetOrCreateInputBindingSet(const BufferGroup* bufferGroup)
{
    if (m_SpecializeInputAssemblerTexCoords)
    {
        if (bufferGroup->texCoordFormat != TexCoordFormat::Unorm16)
            return m_InputBindingSetFloat;
        return m_InputBindingSetUnorm;
    }

    auto it = m_InputBindingSets.find(bufferGroup);
    if (it == m_InputBindingSets.end())
    {
        auto bindingSet = CreateInputBindingSet(bufferGroup);
        m_InputBindingSets[bufferGroup] = bindingSet;
        return bindingSet;
    }

    return it->second;
}

void ForwardShadingPass::SetPushConstants(
    donut::render::GeometryPassContext& abstractContext,
    nvrhi::ICommandList* commandList,
    nvrhi::GraphicsState& state,
    nvrhi::DrawArguments& args)
{
    auto& context = static_cast<Context&>(abstractContext);

    if (!m_SpecializeInputAssemblerTexCoords)
    {
        // Preserve the original raw/custom path, including all per-draw fields.
        ForwardPushConstants constants = {};
        constants.startInstanceLocation = args.startInstanceLocation;
        constants.startVertexLocation = args.startVertexLocation;
        constants.positionOffset = context.positionOffset;
        constants.texCoordOffset = context.texCoordOffset;
        constants.texCoordFormat = uint32_t(context.texCoordFormat);
        constants.texCoordScaleBias = float4(1.f, 1.f, 0.f, 0.f);
        if (context.texCoordFormat == TexCoordFormat::Unorm16 && context.inputBuffers)
        {
            const uint32_t rangeHint = context.geometry ? context.geometry->texCoordDecodeRangeIndex : ~0u;
            const auto& decode = context.inputBuffers->getTexCoordDecodeRange(args.startVertexLocation, rangeHint).texCoord1;
            constants.texCoordScaleBias = float4(decode.scale, decode.offset);
        }
        constants.normalOffset = context.normalOffset;
        constants.tangentOffset = context.tangentOffset;

        commandList->setPushConstants(&constants, sizeof(constants));

        if (!m_UseInputAssembler)
        {
            args.startInstanceLocation = 0;
            args.startVertexLocation = 0;
        }
        return;
    }

    if (context.texCoordFormat != TexCoordFormat::Unorm16)
        return;

    ForwardPushConstants constants = {};
    constants.texCoordFormat = uint32_t(TexCoordFormat::Unorm16);
    constants.texCoordScaleBias = float4(1.f, 1.f, 0.f, 0.f);
    if (context.inputBuffers)
    {
        const uint32_t rangeHint = context.geometry ? context.geometry->texCoordDecodeRangeIndex : ~0u;
        const auto& decode = context.inputBuffers->getTexCoordDecodeRange(args.startVertexLocation, rangeHint).texCoord1;
        constants.texCoordScaleBias = float4(decode.scale, decode.offset);
    }

    // Only RenderView enables this cache, and it invalidates it after every
    // graphics-state change as required by NVRHI. Manual users always write.
    if (context.enablePushConstantCaching && context.pushConstantsValid
        && std::memcmp(&context.lastTexCoordScaleBias, &constants.texCoordScaleBias, sizeof(float4)) == 0)
        return;
    context.lastTexCoordScaleBias = constants.texCoordScaleBias;
    context.pushConstantsValid = context.enablePushConstantCaching;
    commandList->setPushConstants(&constants, sizeof(constants));
}
