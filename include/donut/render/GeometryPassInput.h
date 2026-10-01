// Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <donut/engine/SceneTypes.h>
#include <nvrhi/nvrhi.h>
#include <cstring>
#include <type_traits>
#include <unordered_map>

namespace donut::engine
{
    class ShaderFactory;
}

namespace donut::render
{
    // Stock passes share IA bindings and omit unused floating-point UV constants.
    // Custom passes retain their virtual input factories and full draw constants.
    enum class GeometryInputPolicy { Stock, Custom };

    namespace detail
    {
        // Stock input shaders, layouts and bindings must stay compatible with the
        // optimized draw path. Use a public Custom*Pass alias to replace these hooks.
        // The wrapper adds no state or forwarding in the per-draw methods.
        template<class Base>
        class StockGeometryPass : public Base
        {
        public:
            using Base::Base;
            using CreateParameters = typename Base::CreateParameters;

        protected:
            nvrhi::ShaderHandle CreateVertexShader(engine::ShaderFactory& shaderFactory,
                const CreateParameters& params) override final
            {
                return Base::CreateVertexShader(shaderFactory, params);
            }

            nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* vertexShader,
                const CreateParameters& params) override final
            {
                return Base::CreateInputLayout(vertexShader, params);
            }

            nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* vertexShader,
                const CreateParameters& params, engine::TexCoordFormat texCoordFormat) override final
            {
                return Base::CreateInputLayout(vertexShader, params, texCoordFormat);
            }

            nvrhi::BindingLayoutHandle CreateInputBindingLayout() override final
            {
                return Base::CreateInputBindingLayout();
            }

            nvrhi::BindingSetHandle CreateInputBindingSet(const engine::BufferGroup* buffers) override final
            {
                return Base::CreateInputBindingSet(buffers);
            }
        };
    }

    template<class Configuration, class = void>
    struct IsGeometryPassInputConfiguration : std::false_type {};

    template<class Configuration>
    struct IsGeometryPassInputConfiguration<Configuration, std::void_t<
        typename Configuration::PushConstants,
        decltype(Configuration::InputSpace), decltype(Configuration::PushConstantBinding),
        decltype(Configuration::InstanceBinding), decltype(Configuration::VertexBinding),
        decltype(Configuration::Visibility), decltype(Configuration::HasNormals),
        decltype(Configuration::HasPrevPosition)>> : std::true_type {};

    // Shared implementation for IGeometryPass input handling. Each pass supplies a
    // complete configuration; there are no inherited defaults for binding constants.
    template<GeometryInputPolicy InputPolicy>
    class GeometryPassInput
    {
        static_assert(InputPolicy == GeometryInputPolicy::Stock || InputPolicy == GeometryInputPolicy::Custom,
            "Select the Stock or Custom geometry input policy");

        nvrhi::BindingSetHandle m_FloatBindingSet;
        nvrhi::BindingSetHandle m_UnormBindingSet;
        std::unordered_map<const engine::BufferGroup*, nvrhi::BindingSetHandle> m_BindingSets;

        template<class Context>
        static math::float4 GetScaleBias(const Context& context, uint32_t vertex)
        {
            if (context.texCoordFormat == engine::TexCoordFormat::Unorm16 && context.inputBuffers)
            {
                const uint32_t hint = context.geometry ? context.geometry->texCoordDecodeRangeIndex : ~0u;
                const auto& decode = context.inputBuffers->getTexCoordDecodeRange(vertex, hint).texCoord1;
                return math::float4(decode.scale, decode.offset);
            }
            return math::float4(1.f, 1.f, 0.f, 0.f);
        }

    public:
        nvrhi::ShaderHandle floatVertexShader;
        nvrhi::BindingLayoutHandle floatBindingLayout;

        template<class Configuration, class ShaderLoader>
        void Init(bool useInputAssembler, ShaderLoader&& loadFloatShader)
        {
            static_assert(IsGeometryPassInputConfiguration<Configuration>::value,
                "Geometry input configuration must declare its push constants, bindings, visibility and vertex fields");
            floatVertexShader = nullptr;
            floatBindingLayout = nullptr;
            m_FloatBindingSet = nullptr;
            m_UnormBindingSet = nullptr;
            ResetBindingCache();

            if constexpr (InputPolicy == GeometryInputPolicy::Stock)
            {
                if (useInputAssembler)
                    floatVertexShader = loadFloatShader();
            }
        }

        bool UsesSpecializedInput() const
        {
            if constexpr (InputPolicy == GeometryInputPolicy::Stock)
                // Preserve the generic path if the optimized shader is unavailable.
                return floatVertexShader != nullptr;
            else
                return false;
        }

        template<class Configuration>
        void InitBindingSet(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout)
        {
            if constexpr (InputPolicy == GeometryInputPolicy::Stock)
                if (UsesSpecializedInput())
                {
                    floatBindingLayout = device->createBindingLayout(nvrhi::BindingLayoutDesc()
                        .setVisibility(Configuration::Visibility)
                        .setRegisterSpaceAndDescriptorSet(Configuration::InputSpace));
                    m_FloatBindingSet = device->createBindingSet(nvrhi::BindingSetDesc(), floatBindingLayout);
                    m_UnormBindingSet = CreateBindingSet<Configuration>(device, layout, nullptr, true, false);
                }
        }

        template<class Configuration>
        static nvrhi::BindingLayoutHandle CreateBindingLayout(nvrhi::IDevice* device, bool useInputAssembler, bool isDX11)
        {
            auto desc = nvrhi::BindingLayoutDesc()
                .setVisibility(Configuration::Visibility)
                .setRegisterSpaceAndDescriptorSet(Configuration::InputSpace)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(Configuration::PushConstantBinding, sizeof(typename Configuration::PushConstants)));
            if (!useInputAssembler)
                desc.addItem(isDX11
                    ? nvrhi::BindingLayoutItem::RawBuffer_SRV(Configuration::InstanceBinding)
                    : nvrhi::BindingLayoutItem::StructuredBuffer_SRV(Configuration::InstanceBinding))
                    .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(Configuration::VertexBinding));
            return device->createBindingLayout(desc);
        }

        template<class Configuration>
        static nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout,
            const engine::BufferGroup* buffers, bool useInputAssembler, bool isDX11)
        {
            auto desc = nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(
                Configuration::PushConstantBinding, sizeof(typename Configuration::PushConstants)));
            if (!useInputAssembler)
                desc.addItem(isDX11
                    ? nvrhi::BindingSetItem::RawBuffer_SRV(Configuration::InstanceBinding, buffers->instanceBuffer)
                    : nvrhi::BindingSetItem::StructuredBuffer_SRV(Configuration::InstanceBinding, buffers->instanceBuffer))
                    .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(Configuration::VertexBinding, buffers->vertexBuffer));
            return device->createBindingSet(desc, layout);
        }

        template<class BindingFactory>
        nvrhi::BindingSetHandle GetBindingSet(const engine::BufferGroup* buffers, BindingFactory&& createBindingSet)
        {
            if constexpr (InputPolicy == GeometryInputPolicy::Stock)
                if (UsesSpecializedInput())
                    return buffers->texCoordFormat == engine::TexCoordFormat::Unorm16 ? m_UnormBindingSet : m_FloatBindingSet;

            auto it = m_BindingSets.find(buffers);
            if (it == m_BindingSets.end())
            {
                auto bindings = createBindingSet(buffers);
                m_BindingSets[buffers] = bindings;
                return bindings;
            }
            return it->second;
        }

        void ResetBindingCache() { m_BindingSets.clear(); }

        // Inline into the pass's existing virtual method to avoid another call per draw.
        template<class Configuration, class Context>
#if defined(_MSC_VER)
        __forceinline
#elif defined(__GNUC__)
        __attribute__((always_inline))
#endif
        void SetPushConstants(Context& context, nvrhi::ICommandList* commandList,
            nvrhi::DrawArguments& args, bool useInputAssembler) const
        {
            using Constants = typename Configuration::PushConstants;
            if constexpr (InputPolicy == GeometryInputPolicy::Stock)
            {
                if (UsesSpecializedInput())
                {
                    if (context.texCoordFormat != engine::TexCoordFormat::Unorm16)
                        return;
                    Constants constants = {};
                    constants.texCoordFormat = uint32_t(engine::TexCoordFormat::Unorm16);
                    constants.texCoordScaleBias = GetScaleBias(context, args.startVertexLocation);
                    // RenderView invalidates this cache on every graphics-state change.
                    // Direct pass callers always write their constants.
                    if (context.enablePushConstantCaching && context.pushConstantsValid
                        && std::memcmp(&context.lastTexCoordScaleBias, &constants.texCoordScaleBias, sizeof(math::float4)) == 0)
                        return;
                    context.lastTexCoordScaleBias = constants.texCoordScaleBias;
                    context.pushConstantsValid = context.enablePushConstantCaching;
                    commandList->setPushConstants(&constants, sizeof(constants));
                    return;
                }
            }

            Constants constants = {};
            constants.startInstanceLocation = args.startInstanceLocation;
            constants.startVertexLocation = args.startVertexLocation;
            constants.positionOffset = context.positionOffset;
            constants.texCoordOffset = context.texCoordOffset;
            constants.texCoordFormat = uint32_t(context.texCoordFormat);
            constants.texCoordScaleBias = GetScaleBias(context, args.startVertexLocation);
            if constexpr (Configuration::HasPrevPosition)
                constants.prevPositionOffset = context.prevPositionOffset;
            if constexpr (Configuration::HasNormals)
            {
                constants.normalOffset = context.normalOffset;
                constants.tangentOffset = context.tangentOffset;
            }
            commandList->setPushConstants(&constants, sizeof(constants));
            if (!useInputAssembler)
            {
                args.startInstanceLocation = 0;
                args.startVertexLocation = 0;
            }
        }
    };
}
