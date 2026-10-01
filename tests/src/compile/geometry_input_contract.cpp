// Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
// SPDX-License-Identifier: MIT

#include <donut/render/DepthPass.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/render/GBufferFillPass.h>

using namespace donut::render;
using donut::engine::BufferGroup;
using donut::engine::ShaderFactory;
using donut::engine::TexCoordFormat;

#ifndef OMIT_CONFIG_FIELD
#define OMIT_CONFIG_FIELD 0
#endif
#ifndef OVERRIDE_INPUT_HOOK
#define OVERRIDE_INPUT_HOOK 0
#endif

struct Configuration
{
#if OMIT_CONFIG_FIELD != 1
    using PushConstants = uint32_t;
#endif
#if OMIT_CONFIG_FIELD != 2
    static constexpr uint32_t InputSpace = 0;
#endif
#if OMIT_CONFIG_FIELD != 3
    static constexpr uint32_t PushConstantBinding = 0;
#endif
#if OMIT_CONFIG_FIELD != 4
    static constexpr uint32_t InstanceBinding = 0;
#endif
#if OMIT_CONFIG_FIELD != 5
    static constexpr uint32_t VertexBinding = 1;
#endif
#if OMIT_CONFIG_FIELD != 6
    static constexpr auto Visibility = nvrhi::ShaderType::Vertex;
#endif
#if OMIT_CONFIG_FIELD != 7
    static constexpr bool HasNormals = false;
#endif
#if OMIT_CONFIG_FIELD != 8
    static constexpr bool HasPrevPosition = false;
#endif
};

// Instantiate the actual initialization contract; checking the detection trait
// alone would not catch a missing static_assert in the production implementation.
void CheckConfiguration()
{
    GeometryPassInput<GeometryInputPolicy::Stock> stock;
    stock.Init<Configuration>(true, [] { return nvrhi::ShaderHandle{}; });
    GeometryPassInput<GeometryInputPolicy::Custom> custom;
    custom.Init<Configuration>(true, [] { return nvrhi::ShaderHandle{}; });
}

template<class Base>
struct InputExtension : Base
{
    using Parameters = typename Base::CreateParameters;
#if OVERRIDE_INPUT_HOOK == 1 || OVERRIDE_INPUT_HOOK == 0
    nvrhi::ShaderHandle CreateVertexShader(ShaderFactory&, const Parameters&) override;
#endif
#if OVERRIDE_INPUT_HOOK == 2 || OVERRIDE_INPUT_HOOK == 0
    nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader*, const Parameters&) override;
#endif
#if OVERRIDE_INPUT_HOOK == 3 || OVERRIDE_INPUT_HOOK == 0
    nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader*, const Parameters&, TexCoordFormat) override;
#endif
#if OVERRIDE_INPUT_HOOK == 4 || OVERRIDE_INPUT_HOOK == 0
    nvrhi::BindingLayoutHandle CreateInputBindingLayout() override;
#endif
#if OVERRIDE_INPUT_HOOK == 5 || OVERRIDE_INPUT_HOOK == 0
    nvrhi::BindingSetHandle CreateInputBindingSet(const BufferGroup*) override;
#endif
};

#if OVERRIDE_INPUT_HOOK != 0
#if STOCK_PASS == 0
using StockPass = DepthPass;
#elif STOCK_PASS == 1
using StockPass = ForwardShadingPass;
#else
using StockPass = GBufferFillPass;
#endif
static_assert(sizeof(InputExtension<StockPass>) > 0);
#else
static_assert(sizeof(InputExtension<CustomDepthPass>) > 0);
static_assert(sizeof(InputExtension<CustomForwardShadingPass>) > 0);
static_assert(sizeof(InputExtension<CustomGBufferFillPass>) > 0);

struct DepthPixelExtension : DepthPass
{
    nvrhi::ShaderHandle CreatePixelShader(ShaderFactory&, const CreateParameters&) override;
};
struct ForwardPixelExtension : ForwardShadingPass
{
    nvrhi::ShaderHandle CreatePixelShader(ShaderFactory&, const CreateParameters&, bool) override;
};
struct GBufferPixelExtension : GBufferFillPass
{
    nvrhi::ShaderHandle CreatePixelShader(ShaderFactory&, const CreateParameters&, bool) override;
};
#endif
