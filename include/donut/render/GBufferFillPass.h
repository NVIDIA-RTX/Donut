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

#pragma once

#include <donut/engine/View.h>
#include <donut/engine/SceneTypes.h>
#include <donut/render/GeometryPasses.h>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class MaterialBindingCache;
    struct Material;
}

namespace donut::render
{
    class GBufferFillPass : public IGeometryPass
    {
    public:
        union PipelineKey
        {
            struct
            {
                nvrhi::RasterCullMode cullMode : 2;
                bool alphaTested : 1;
                bool frontCounterClockwise : 1;
                bool reverseDepth : 1;
                // Match the other bitfield storage sizes so MSVC keeps the key in one word.
                uint8_t texCoordFormat : 2;
            } bits;
            uint32_t value;

            static constexpr size_t Count = 1 << 7;
        };
        static_assert(sizeof(PipelineKey) == sizeof(uint32_t), "PipelineKey bits must fit its cache key");

        class Context : public GeometryPassContext
        {
        public:
            nvrhi::BindingSetHandle inputBindingSet;
            const engine::BufferGroup* inputBuffers = nullptr;
            PipelineKey keyTemplate;

            uint32_t positionOffset = 0;
            uint32_t prevPositionOffset = 0;
            uint32_t texCoordOffset = 0;
            engine::TexCoordFormat texCoordFormat = engine::TexCoordFormat::Float32;
            uint32_t normalOffset = 0;
            uint32_t tangentOffset = 0;

            // Keep the raw input fields together; only specialized UNORM IA uses this cache.
            dm::float4 lastTexCoordScaleBias = dm::float4(1.f, 1.f, 0.f, 0.f);

            Context()
            {
                keyTemplate.value = 0;
            }
        };

        struct CreateParameters
        {
            std::shared_ptr<engine::MaterialBindingCache> materialBindings;
            bool enableSinglePassCubemap = false;
            bool enableDepthWrite = true;
            bool enableMotionVectors = false;
            bool trackLiveness = true;

            // Switches between loading vertex data through the Input Assembler (true) or buffer SRVs (false).
            // Using Buffer SRVs is often faster.
            bool useInputAssembler = false;

            // Share IA bindings and omit unused floating-point UV constants.
            // Disable for custom input shaders/layouts/bindings or shader stages
            // and draw code that require the full input push constants.
            bool enableTexCoordOptimizations = true;

            uint32_t stencilWriteMask = 0;
            uint32_t numConstantBufferVersions = 16;
        };

    protected:
        nvrhi::DeviceHandle m_Device;
        // Indexed by TexCoordFormat to cache layouts for mixed-format scenes.
        std::array<nvrhi::InputLayoutHandle, size_t(engine::TexCoordFormat::Count)> m_InputLayouts;
        CreateParameters m_CreateParameters;
        // General shader for UV decoding, buffer loads, or custom input behavior.
        nvrhi::ShaderHandle m_VertexShader;
        // FP32/FP16 input-assembler variant without UV decoding or input push constants.
        nvrhi::ShaderHandle m_OptimizedFloatVertexShader;
        nvrhi::BindingSetHandle m_UnormInputBindingSet;
        std::unordered_map<const engine::BufferGroup*, nvrhi::BindingSetHandle> m_InputBindingSets;
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::ShaderHandle m_PixelShaderAlphaTested;
        nvrhi::ShaderHandle m_GeometryShader;
        nvrhi::BindingLayoutHandle m_InputBindingLayout;
        nvrhi::BindingLayoutHandle m_ViewBindingLayout;
        nvrhi::BindingSetHandle m_ViewBindings;
        nvrhi::BufferHandle m_GBufferCB;
        engine::ViewType::Enum m_SupportedViewTypes = engine::ViewType::PLANAR;
        nvrhi::GraphicsPipelineHandle m_Pipelines[PipelineKey::Count];
        std::mutex m_Mutex;

        std::shared_ptr<engine::CommonRenderPasses> m_CommonPasses;
        std::shared_ptr<engine::MaterialBindingCache> m_MaterialBindings;

        bool m_EnableDepthWrite = true;
        bool m_EnableMotionVectors = false;
        bool m_IsDX11 = false;
        bool m_UseInputAssembler = false;
        uint32_t m_StencilWriteMask = 0;
        
        virtual nvrhi::ShaderHandle CreateVertexShader(engine::ShaderFactory& shaderFactory, const CreateParameters& params);
        virtual nvrhi::ShaderHandle CreateGeometryShader(engine::ShaderFactory& shaderFactory, const CreateParameters& params);
        virtual nvrhi::ShaderHandle CreatePixelShader(engine::ShaderFactory& shaderFactory, const CreateParameters& params, bool alphaTested);
        // Legacy FP32 layout hook; 16-bit formats use the format-aware overload below.
        virtual nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params);
        // Override this overload to customize layouts for all texture coordinate formats.
        virtual nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params, engine::TexCoordFormat texCoordFormat);
        virtual nvrhi::BindingLayoutHandle CreateInputBindingLayout();
        virtual nvrhi::BindingSetHandle CreateInputBindingSet(const engine::BufferGroup* bufferGroup);
        virtual void CreateViewBindings(nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set, const CreateParameters& params);
        virtual std::shared_ptr<engine::MaterialBindingCache> CreateMaterialBindingCache(engine::CommonRenderPasses& commonPasses);
        virtual nvrhi::GraphicsPipelineHandle CreateGraphicsPipeline(PipelineKey key, nvrhi::FramebufferInfo const& framebufferInfo);
        nvrhi::BindingSetHandle GetOrCreateInputBindingSet(const engine::BufferGroup* bufferGroup);
        
    public:
        GBufferFillPass(nvrhi::IDevice* device, std::shared_ptr<engine::CommonRenderPasses> commonPasses);

        virtual void Init(
            engine::ShaderFactory& shaderFactory,
            const CreateParameters& params);

        void ResetBindingCache();
        
        // IGeometryPass implementation

        [[nodiscard]] engine::ViewType::Enum GetSupportedViewTypes() const override;
        void SetupView(GeometryPassContext& context, nvrhi::ICommandList* commandList, const engine::IView* view, const engine::IView* viewPrev) override;
        bool SetupMaterial(GeometryPassContext& context, const engine::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state) override;
        void SetupInputBuffers(GeometryPassContext& context, const engine::BufferGroup* buffers, nvrhi::GraphicsState& state) override;
        void SetPushConstants(GeometryPassContext& context, nvrhi::ICommandList* commandList, nvrhi::GraphicsState& state, nvrhi::DrawArguments& args) override;
    };

    class MaterialIDPass : public GBufferFillPass
    {
    protected:
        nvrhi::ShaderHandle CreatePixelShader(engine::ShaderFactory& shaderFactory, const CreateParameters& params, bool alphaTested) override;

    public:
        using GBufferFillPass::GBufferFillPass;

        void Init(
            engine::ShaderFactory& shaderFactory,
            const CreateParameters& params) override;
    };
}
