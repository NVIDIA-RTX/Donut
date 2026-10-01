// Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
// SPDX-License-Identifier: MIT

#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/math/float.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/MaterialBindingCache.h>
#include <donut/engine/SceneGraph.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>
#include <donut/render/DepthPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/tests/GpuTestLog.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace donut;
using namespace donut::math;
using namespace donut::engine;
using namespace donut::render;
using donut::tests::LogErrorCounter;
#include <donut/shaders/bindless.h>

namespace
{
    constexpr uint32_t Width = 160;
    constexpr uint32_t Height = 32;
    constexpr size_t GroupCount = 5;

    nvrhi::ShaderHandle CreateTestPixelShader(ShaderFactory& factory, const char* entry = "scene")
    {
        return factory.CreateShader("tests/texcoord_raster.hlsl", entry, nullptr, nvrhi::ShaderType::Pixel);
    }

    template<typename Base>
    class TestDepthPassT : public Base
    {
    public:
        using Base::Base;
        using CreateParameters = typename Base::CreateParameters;
        void ClearMaterialBindings() { this->m_MaterialBindings->Clear(); }

    protected:
        nvrhi::ShaderHandle CreatePixelShader(ShaderFactory& factory, const CreateParameters&) override
        {
            return CreateTestPixelShader(factory, "depth");
        }
    };

    class TestDepthLayoutPass : public TestDepthPassT<CustomDepthPass>
    {
    public:
        using TestDepthPassT<CustomDepthPass>::TestDepthPassT;
        uint32_t float16LayoutCount = 0;
        uint32_t unorm16LayoutCount = 0;

    protected:
        nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* shader, const CreateParameters& params, TexCoordFormat format) override
        {
            if (format == TexCoordFormat::Float16)
                ++float16LayoutCount;
            if (format == TexCoordFormat::Unorm16)
                ++unorm16LayoutCount;
            return CustomDepthPass::CreateInputLayout(shader, params, format);
        }
    };

    struct InputFactoryChecks
    {
        nvrhi::BindingLayoutHandle createdLayout;
        std::unordered_map<const BufferGroup*, nvrhi::BindingSetHandle> createdSets;
        uint32_t layoutCalls = 0;
        bool receivedNonNullBuffers = true;

        bool CheckInputFactories(const BufferGroup* buffers, const nvrhi::GraphicsState& state) const
        {
            const auto found = createdSets.find(buffers);
            if (!receivedNonNullBuffers || layoutCalls == 0 || !createdLayout
                || found == createdSets.end() || !found->second)
                return false;
            bool usesLayout = false;
            for (const auto& layout : state.pipeline->getDesc().bindingLayouts)
                usesLayout |= layout.Get() == createdLayout.Get();
            bool usesSet = false;
            for (const auto* binding : state.bindings)
                usesSet |= binding == found->second.Get();
            return usesLayout && usesSet && found->second->getLayout() == createdLayout.Get();
        }
    };

    // Exercise the application extension points independently of their default
    // implementations, and verify that the returned objects reach the draw.
    template<typename Base>
    class TestInputFactories : public Base, public InputFactoryChecks
    {
    public:
        using Base::Base;

    protected:
        nvrhi::BindingLayoutHandle CreateInputBindingLayout() override
        {
            ++layoutCalls;
            createdLayout = Base::CreateInputBindingLayout();
            return createdLayout;
        }

        nvrhi::BindingSetHandle CreateInputBindingSet(const BufferGroup* buffers) override
        {
            receivedNonNullBuffers &= buffers != nullptr;
            if (!buffers)
                return nullptr;
            auto bindings = Base::CreateInputBindingSet(buffers);
            createdSets[buffers] = bindings;
            return bindings;
        }
    };

    template<typename Base>
    class TestForwardPassT : public Base
    {
    public:
        using Base::Base;
        using CreateParameters = typename Base::CreateParameters;
        void ClearMaterialBindings() { this->m_MaterialBindings->Clear(); }

    protected:
        nvrhi::ShaderHandle CreatePixelShader(ShaderFactory& factory, const CreateParameters&, bool) override
        {
            return CreateTestPixelShader(factory);
        }
    };

    template<typename Base>
    class TestGBufferPassT : public Base
    {
    public:
        using Base::Base;
        using CreateParameters = typename Base::CreateParameters;
        void ClearMaterialBindings() { this->m_MaterialBindings->Clear(); }

    protected:
        nvrhi::ShaderHandle CreatePixelShader(ShaderFactory& factory, const CreateParameters&, bool) override
        {
            return CreateTestPixelShader(factory);
        }
    };

    // Existing applications can override the vertex shader. The stock fast path
    // must not silently replace their shader or omit its required constants.
    class LegacyDepthPass : public CustomDepthPass
    {
    public:
        using CustomDepthPass::CustomDepthPass;
        void ClearMaterialBindings() { m_MaterialBindings->Clear(); }
        uint32_t inputBindingCount = 0;
        bool inputBindingsReceivedBuffers = true;

    protected:
        nvrhi::ShaderHandle CreateVertexShader(ShaderFactory& factory, const CreateParameters& params) override
        {
            return params.useInputAssembler
                ? factory.CreateShader("tests/texcoord_raster.hlsl", "custom_depth", nullptr, nvrhi::ShaderType::Vertex)
                : CustomDepthPass::CreateVertexShader(factory, params);
        }

        nvrhi::ShaderHandle CreatePixelShader(ShaderFactory& factory, const CreateParameters&) override
        {
            return CreateTestPixelShader(factory, "depth");
        }

        nvrhi::BindingSetHandle CreateInputBindingSet(const BufferGroup* buffers) override
        {
            // Custom factories may use buffer-group data even with the IA path.
            ++inputBindingCount;
            inputBindingsReceivedBuffers &= buffers != nullptr;
            return buffers ? CustomDepthPass::CreateInputBindingSet(buffers) : nullptr;
        }
    };

    struct Fixture
    {
        std::shared_ptr<SceneGraph> graph = std::make_shared<SceneGraph>();
        std::shared_ptr<Material> material = std::make_shared<Material>();
        std::shared_ptr<Material> alternateMaterial;
        std::array<std::shared_ptr<MeshInstance>, GroupCount> instances;
        std::array<DrawItem, GroupCount> draws{};
        std::array<float2, GroupCount> expected;
        nvrhi::TextureHandle color;
        nvrhi::TextureHandle alternateColor;
        nvrhi::TextureHandle depth;
        nvrhi::FramebufferHandle framebuffer;
        nvrhi::FramebufferHandle alternateFramebuffer;
        nvrhi::StagingTextureHandle readback;
        PlanarView view;

        Fixture(nvrhi::IDevice* device, nvrhi::ICommandList* commands, const CommonRenderPasses& common)
        {
            graph->SetRootNode(std::make_shared<SceneGraphNode>());

            // Alpha-tested depth draws keep the production depth pass's pixel stage active.
            material->domain = MaterialDomain::AlphaTested;
            material->baseOrDiffuseTexture = std::make_shared<LoadedTexture>();
            material->baseOrDiffuseTexture->texture = common.m_WhiteTexture;
            material->baseOrDiffuseColor = float3(0.25f);
            material->materialConstants = device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(sizeof(MaterialConstants)).setIsConstantBuffer(true)
                .enableAutomaticStateTracking(nvrhi::ResourceStates::ConstantBuffer));
            MaterialConstants materialConstants{};
            material->FillConstantBuffer(materialConstants);
            commands->writeBuffer(material->materialConstants, &materialConstants, sizeof(materialConstants));
            alternateMaterial = std::make_shared<Material>(*material);
            alternateMaterial->baseOrDiffuseColor = float3(0.75f);
            alternateMaterial->materialConstants = device->createBuffer(material->materialConstants->getDesc());
            alternateMaterial->FillConstantBuffer(materialConstants);
            commands->writeBuffer(alternateMaterial->materialConstants, &materialConstants, sizeof(materialConstants));

            std::array<InstanceData, GroupCount> instanceData{};
            for (auto& instance : instanceData)
            {
                instance.transform = float3x4::identity();
                instance.prevTransform = float3x4::identity();
            }
            auto instanceDesc = nvrhi::BufferDesc().setByteSize(sizeof(instanceData))
                .setIsVertexBuffer(true).setCanHaveRawViews(true)
                .enableAutomaticStateTracking(nvrhi::ResourceStates::Common);
            if (device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11)
                instanceDesc.setStructStride(sizeof(InstanceData));
            auto instanceBuffer = device->createBuffer(instanceDesc);
            commands->writeBuffer(instanceBuffer, instanceData.data(), sizeof(instanceData));

            const std::array<float2, GroupCount> baseUV = { float2(0.25f, 0.5f), float2(-0.75f, 1.5f),
                float2(3000.f, -12.5f), float2(-8.f, 24.f), float2(1.75f, -0.25f) };
            for (size_t group = 0; group < GroupCount; ++group)
            {
                auto mesh = std::make_shared<MeshInfo>();
                mesh->buffers = group == 3 ? instances[2]->GetMesh()->buffers : std::make_shared<BufferGroup>();
                auto& buffers = *mesh->buffers;
                buffers.texCoordFormat = group == 1 ? TexCoordFormat::Float16 :
                    (group == 2 || group == 3) ? TexCoordFormat::Unorm16 : TexCoordFormat::Float32;
                buffers.instanceBuffer = instanceBuffer;
                mesh->vertexOffset = group == 3 ? 4 : 1; // Shared UNORM buffer, separate decode domains.
                mesh->totalVertices = 3;
                mesh->totalIndices = 3;
                auto geometry = std::make_shared<MeshGeometry>();
                geometry->numIndices = 3;
                geometry->numVertices = 3;
                geometry->material = material;
                mesh->geometries.push_back(geometry);

                float left = -1.f + float(group) * (2.f / GroupCount) + 0.05f;
                float right = -1.f + float(group + 1) * (2.f / GroupCount) - 0.05f;
                float center = (left + right) * 0.5f;
                if (group != 3)
                {
                    std::vector<float3> positions = { float3(9.f), float3(left, -0.8f, 0.5f), float3(right, -0.8f, 0.5f), float3(center, 0.8f, 0.5f) };
                    std::vector<float2> uvs = { float2(9.f), baseUV[group], baseUV[group] + float2(0.25f, 0.f), baseUV[group] + float2(0.f, 0.5f) };
                    if (group == 2)
                    {
                        // The next draw shares the buffer, material and index range, but has a different base vertex.
                        const float nextLeft = left + 2.f / GroupCount;
                        const float nextRight = right + 2.f / GroupCount;
                        positions.insert(positions.end(), { float3(nextLeft, -0.8f, 0.5f), float3(nextRight, -0.8f, 0.5f),
                            float3((nextLeft + nextRight) * 0.5f, 0.8f, 0.5f) });
                        uvs.insert(uvs.end(), { baseUV[3], baseUV[3] + float2(0.25f, 0.f), baseUV[3] + float2(0.f, 0.5f) });
                    }
                    std::vector<uint32_t> normals(positions.size(), 0x007f0000u);
                    std::vector<uint32_t> tangents(positions.size(), 0x7f00007fu);
                    std::vector<uint8_t> vertices(16 + group * 16, 0);
                    auto append = [&](VertexAttribute attribute, const void* data, size_t size)
                    {
                        vertices.resize((vertices.size() + 15) & ~size_t(15));
                        buffers.getVertexBufferRange(attribute) = nvrhi::BufferRange(vertices.size(), size);
                        const auto* bytes = static_cast<const uint8_t*>(data);
                        vertices.insert(vertices.end(), bytes, bytes + size);
                    };
                    append(VertexAttribute::Position, positions.data(), positions.size() * sizeof(float3));
                    append(VertexAttribute::PrevPosition, positions.data(), positions.size() * sizeof(float3));
                    if (buffers.texCoordFormat == TexCoordFormat::Float16)
                    {
                        std::vector<float16_t2> packed(uvs.size());
                        for (size_t vertex = 0; vertex < packed.size(); ++vertex)
                            packed[vertex] = Float32ToFloat16x2(uvs[vertex]);
                        append(VertexAttribute::TexCoord1, packed.data(), packed.size() * sizeof(float16_t2));
                    }
                    else if (buffers.texCoordFormat == TexCoordFormat::Unorm16)
                    {
                        std::vector<uint32_t> packed(uvs.size(), 0);
                        for (uint32_t domain = 0; domain < 2; ++domain)
                        {
                            TexCoordDecodeRange range;
                            range.vertexOffset = 1 + domain * 3;
                            range.numVertices = 3;
                            range.texCoord1.scale = float2(0.25f, 0.5f);
                            range.texCoord1.offset = baseUV[2 + domain];
                            buffers.texCoordDecodeRanges.push_back(range);
                            packed[range.vertexOffset + 1] = 0x0000ffffu;
                            packed[range.vertexOffset + 2] = 0xffff0000u;
                        }
                        append(VertexAttribute::TexCoord1, packed.data(), packed.size() * sizeof(uint32_t));
                    }
                    else
                        append(VertexAttribute::TexCoord1, uvs.data(), uvs.size() * sizeof(float2));
                    append(VertexAttribute::Normal, normals.data(), normals.size() * sizeof(uint32_t));
                    append(VertexAttribute::Tangent, tangents.data(), tangents.size() * sizeof(uint32_t));

                    buffers.vertexBuffer = device->createBuffer(nvrhi::BufferDesc().setByteSize(vertices.size())
                        .setIsVertexBuffer(true).setCanHaveRawViews(true)
                        .enableAutomaticStateTracking(nvrhi::ResourceStates::Common));
                    commands->writeBuffer(buffers.vertexBuffer, vertices.data(), vertices.size());
                    const uint32_t indices[] = { 0, 1, 2 };
                    buffers.indexBuffer = device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(indices))
                        .setIsIndexBuffer(true).enableAutomaticStateTracking(nvrhi::ResourceStates::IndexBuffer));
                    commands->writeBuffer(buffers.indexBuffer, indices, sizeof(indices));
                }

                instances[group] = std::make_shared<MeshInstance>(mesh);
                geometry->texCoordDecodeRangeIndex = buffers.getTexCoordDecodeRangeIndex(mesh->vertexOffset);
                graph->AttachLeafNode(graph->GetRootNode(), instances[group]);
                draws[group] = { instances[group].get(), mesh.get(), geometry.get(), material.get(), &buffers, 0.f, nvrhi::RasterCullMode::None, nullptr };

                // Reference interpolation at the center pixel of each screen region.
                float sampleX = (float(16 + 32 * group) + 0.5f) * (2.f / Width) - 1.f;
                float sampleY = 1.f - (16.f + 0.5f) * (2.f / Height);
                float weightTop = (sampleY + 0.8f) / 1.6f;
                float weightRight = (sampleX - left - weightTop * (center - left)) / (right - left);
                expected[group] = baseUV[group] + float2(0.25f * weightRight, 0.5f * weightTop);
            }
            graph->Refresh(0);

            auto colorDesc = nvrhi::TextureDesc().setWidth(Width).setHeight(Height)
                .setFormat(nvrhi::Format::RGBA32_FLOAT).setIsRenderTarget(true)
                .enableAutomaticStateTracking(nvrhi::ResourceStates::RenderTarget);
            color = device->createTexture(colorDesc);
            alternateColor = device->createTexture(colorDesc);
            readback = device->createStagingTexture(colorDesc, nvrhi::CpuAccessMode::Read);
            depth = device->createTexture(nvrhi::TextureDesc().setWidth(Width).setHeight(Height)
                .setFormat(nvrhi::Format::D32).setIsTypeless(true).setIsRenderTarget(true)
                .enableAutomaticStateTracking(nvrhi::ResourceStates::DepthWrite));
            framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(color).setDepthAttachment(depth));
            alternateFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(alternateColor).setDepthAttachment(depth));
            view.SetViewport(nvrhi::Viewport(float(Width), float(Height)));
            view.SetMatrices(affine3::identity(), float4x4::identity());
            view.UpdateCache();
        }

        bool RenderAndCheck(nvrhi::IDevice* device, IGeometryPass& pass, GeometryPassContext& context, const char* name,
            const std::function<void(nvrhi::ICommandList*)>& prepareLights = {}, bool stressStateChanges = false, bool manualDraws = false,
            uint32_t viewRepetitions = 1, const std::function<void()>& resetBindings = {}, bool expectSpecializedInput = false,
            const std::function<bool(const BufferGroup*, const nvrhi::GraphicsState&)>& checkInputBindings = {})
        {
            bool passResult = true;
            std::vector<DrawItem> sequence(draws.begin(), draws.end());
            if (stressStateChanges)
            {
                // Include same-format buffer switches as well as material changes
                // within the shared UNORM buffer.
                sequence = { draws[0], draws[4], draws[0], draws[1], draws[2], draws[2], draws[2], draws[3], draws[3], draws[2], draws[4] };
                sequence[6].material = alternateMaterial.get();
                sequence[8].material = alternateMaterial.get();
            }
            if (resetBindings)
                sequence = { draws[0], draws[4], draws[1], draws[2], draws[3] };
            std::array<float, GroupCount> expectedMaterial{};
            for (const DrawItem& item : sequence)
                for (size_t group = 0; group < GroupCount; ++group)
                    if (item.instance == instances[group].get())
                        expectedMaterial[group] = item.material->baseOrDiffuseColor.x;
            if (resetBindings)
                for (size_t group = 1; group < GroupCount; ++group)
                    expectedMaterial[group] = alternateMaterial->baseOrDiffuseColor.x;
            auto commands = device->createCommandList();
            commands->open();
            if (prepareLights)
                prepareLights(commands);
            for (uint32_t repetition = 0; repetition < viewRepetitions; ++repetition)
            {
                // A second view uses a distinct but compatible framebuffer with
                // the same pass/context, including through the manual API.
                const auto& targetColor = repetition % 2 ? alternateColor : color;
                const auto& targetFramebuffer = repetition % 2 ? alternateFramebuffer : framebuffer;
                commands->clearTextureFloat(targetColor, nvrhi::AllSubresources, nvrhi::Color(-99.f));
                commands->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 1.f, false, 0);
                if (manualDraws)
                {
                    // Use only the existing public pass contract, without RenderView's
                    // cache notifications. Every setGraphicsState invalidates constants.
                    pass.SetupView(context, commands, &view, &view);
                    for (const DrawItem& item : sequence)
                    {
                        nvrhi::GraphicsState state;
                        state.framebuffer = targetFramebuffer;
                        state.viewport = view.GetViewportState();
                        pass.SetupInputBuffers(context, item.buffers, state);
                        if (!pass.SetupMaterial(context, item.material, item.cullMode, state))
                            return false;
                        if (checkInputBindings && !checkInputBindings(item.buffers, state))
                        {
                            std::fprintf(stderr, "%s: custom input factories were bypassed or their bindings were not used\n", name);
                            passResult = false;
                        }
                        // Check the selected pipeline, not just equivalent rendered UVs:
                        // specialized floating-point IA pipelines must omit input constants.
                        bool hasPushConstants = false;
                        for (const auto& layout : state.pipeline->getDesc().bindingLayouts)
                            if (const auto* desc = layout->getDesc())
                                for (const auto& binding : desc->bindings)
                                    hasPushConstants |= binding.type == nvrhi::ResourceType::PushConstants;
                        const bool expectPushConstants = !expectSpecializedInput
                            || item.buffers->texCoordFormat == TexCoordFormat::Unorm16;
                        if (hasPushConstants != expectPushConstants)
                            std::fprintf(stderr, "%s format %u: expected push constants %u, got %u\n", name,
                                unsigned(item.buffers->texCoordFormat), unsigned(expectPushConstants), unsigned(hasPushConstants));
                        passResult &= hasPushConstants == expectPushConstants;
                        commands->setGraphicsState(state);
                        nvrhi::DrawArguments args;
                        args.vertexCount = item.geometry->numIndices;
                        args.instanceCount = 1;
                        args.startVertexLocation = item.mesh->vertexOffset + item.geometry->vertexOffsetInMesh;
                        args.startIndexLocation = item.mesh->indexOffset + item.geometry->indexOffsetInMesh;
                        args.startInstanceLocation = item.instance->GetInstanceIndex();
                        pass.SetPushConstants(context, commands, state, args);
                        commands->drawIndexed(args);
                    }
                }
                else
                {
                    class ResetDrawStrategy : public PassthroughDrawStrategy
                    {
                        const std::function<void()>& resetBindings;
                        size_t nextIndex = 0;
                    public:
                        explicit ResetDrawStrategy(const std::function<void()>& resetBindings)
                            : resetBindings(resetBindings) { }

                        const DrawItem* GetNextItem() override
                        {
                            if (nextIndex++ == 1 && resetBindings)
                                resetBindings();
                            return PassthroughDrawStrategy::GetNextItem();
                        }
                    } strategy(resetBindings);
                    strategy.SetData(sequence.data(), sequence.size());
                    RenderView(commands, &view, &view, targetFramebuffer, strategy, pass, context);
                }
            }
            commands->copyTexture(readback, {}, viewRepetitions % 2 ? color : alternateColor, {});
            commands->close();
            device->executeCommandList(commands);
            device->waitForIdle();

            size_t rowPitch = 0;
            const auto* pixels = static_cast<const uint8_t*>(device->mapStagingTexture(readback, {}, nvrhi::CpuAccessMode::Read, &rowPitch));
            if (!pixels)
                return false;
            for (size_t group = 0; group < GroupCount; ++group)
            {
                float4 actual;
                std::memcpy(&actual, pixels + 16 * rowPitch + (16 + 32 * group) * sizeof(float4), sizeof(actual));
                // Allow rasterizer interpolation precision without masking incorrect UV strides or bindings.
                const float tolerance = std::abs(expected[group].x) > 1024.f ? 1e-3f : 2e-4f;
                bool matches = std::abs(actual.x - expected[group].x) < tolerance
                    && std::abs(actual.y - expected[group].y) < tolerance
                    && actual.z == expectedMaterial[group] && actual.w == 1.f;
                if (!matches)
                    std::fprintf(stderr, "%s group %zu: expected (%f, %f, %f, 1), got (%f, %f, %f, %f)\n",
                        name, group, expected[group].x, expected[group].y, expectedMaterial[group], actual.x, actual.y, actual.z, actual.w);
                passResult &= matches;
            }
            device->unmapStagingTexture(readback);
            std::printf("%s: %s\n", name, passResult ? "PASS" : "FAIL");
            return passResult;
        }

        template<typename PassType>
        bool ExercisePass(nvrhi::IDevice* device, PassType& pass, GeometryPassContext& context, const char* name,
            bool expectSpecializedInput = false)
        {
            std::function<void(nvrhi::ICommandList*)> prepareLights;
            if constexpr (std::is_base_of_v<ForwardShadingPass, PassType>
                || std::is_base_of_v<CustomForwardShadingPass, PassType>)
                prepareLights = [&](nvrhi::ICommandList* commands)
                {
                    pass.PrepareLights(static_cast<typename PassType::Context&>(context), commands, {}, float3(0.f), float3(0.f), {});
                };
            std::function<bool(const BufferGroup*, const nvrhi::GraphicsState&)> checkInputBindings;
            if constexpr (std::is_base_of_v<InputFactoryChecks, PassType>)
                checkInputBindings = [&](const BufferGroup* buffers, const nvrhi::GraphicsState& state)
                {
                    return pass.CheckInputFactories(buffers, state);
                };
            bool passed = RenderAndCheck(device, pass, context, name, prepareLights);
            // Reuse the pass and context across command lists, and twice in one list.
            passed &= RenderAndCheck(device, pass, context, (std::string(name) + " repeated/state resets").c_str(),
                prepareLights, true, false, 2);

            auto& ranges = instances[2]->GetMesh()->buffers->texCoordDecodeRanges;
            const auto savedRanges = ranges;
            const auto savedExpected = expected;
            // Distinct geometries can share identical decode values. Change the live
            // metadata without rebuilding the pass, then restore it for manual draws.
            expected[2] += float2(0.5f, -0.25f);
            ranges[0].texCoord1.offset += float2(0.5f, -0.25f);
            expected[3] += ranges[0].texCoord1.offset - ranges[1].texCoord1.offset;
            ranges[1].texCoord1 = ranges[0].texCoord1;
            passed &= RenderAndCheck(device, pass, context, (std::string(name) + " equal/live decode").c_str(),
                prepareLights, true);
            ranges = savedRanges;
            expected = savedExpected;
            passed &= RenderAndCheck(device, pass, context, (std::string(name) + " manual API").c_str(),
                prepareLights, true, true, 2, {}, expectSpecializedInput, checkInputBindings);
            // Clear the binding cache inside one RenderView, between two FP32
            // buffers with the same material and pipeline key. The first draw
            // retains its original CB; each later draw must use the replacement.
            const auto originalMaterialBuffer = material->materialConstants;
            passed &= RenderAndCheck(device, pass, context, (std::string(name) + " material cache clear").c_str(),
                prepareLights, false, false, 1, [&]()
                {
                    material->materialConstants = alternateMaterial->materialConstants;
                    // Directly clear the shared cache, independently of the pass's
                    // ResetBindingCache, to require a fresh material binding.
                    pass.ClearMaterialBindings();
                });
            material->materialConstants = originalMaterialBuffer;
            pass.ResetBindingCache();
            return passed;
        }
    };
}

static bool RunGpu(const std::filesystem::path& shaderPath, const std::filesystem::path& testShaderPath,
    nvrhi::GraphicsAPI graphicsApi, bool debugRuntime);

int main(int argc, char** argv)
{
    log::ConsoleApplicationMode();
    log::SetMinSeverity(log::Severity::Warning);
    std::filesystem::path shaderPath;
    std::filesystem::path testShaderPath;
    bool debugRuntime = false;
    for (int arg = 1; arg < argc; ++arg)
    {
        if (std::strcmp(argv[arg], "--gpu") == 0 && arg + 1 < argc)
            shaderPath = argv[++arg];
        else if (std::strcmp(argv[arg], "--test-shaders") == 0 && arg + 1 < argc)
            testShaderPath = argv[++arg];
        else if (std::strcmp(argv[arg], "--debug-runtime") == 0)
            debugRuntime = true;
    }
    if (shaderPath.empty())
    {
        std::puts("Raster GPU tests skipped; use --gpu <Donut platform shader directory> --test-shaders <test platform shader directory> [-dx11|-dx12|-vk] [--debug-runtime].");
        return 77;
    }
    if (testShaderPath.empty())
    {
        std::fputs("--test-shaders is required with --gpu.\n", stderr);
        return 1;
    }

    auto graphicsApi = app::GetGraphicsAPIFromCommandLine(argc, argv);
    LogErrorCounter logErrors(debugRuntime && graphicsApi == nvrhi::GraphicsAPI::VULKAN);
    const bool passed = RunGpu(shaderPath, testShaderPath, graphicsApi, debugRuntime);
    // RunGpu has released its resources and device, so teardown errors also fail.
    logErrors.Report();
    return passed && logErrors.errors.load() == 0 ? 0 : 1;
}

static bool RunGpu(const std::filesystem::path& shaderPath, const std::filesystem::path& testShaderPath,
    nvrhi::GraphicsAPI graphicsApi, bool debugRuntime)
{
    std::unique_ptr<app::DeviceManager> manager(app::DeviceManager::Create(graphicsApi));
    app::DeviceCreationParameters parameters;
    parameters.enableNvrhiValidationLayer = true;
    parameters.enableDebugRuntime = debugRuntime;
#if DONUT_WITH_VULKAN
    if (debugRuntime)
        parameters.ignoredVulkanValidationMessageLocations.clear();
#endif
    if (!manager || !manager->CreateHeadlessDevice(parameters))
        return false;
    if (debugRuntime && graphicsApi == nvrhi::GraphicsAPI::VULKAN)
    {
        if (!manager->IsVulkanLayerEnabled("VK_LAYER_KHRONOS_validation"))
        {
            std::fputs("Requested Khronos Vulkan validation layer is not active.\n", stderr);
            return false;
        }
        std::puts("Khronos Vulkan validation layer: ENABLED (errors checked, all messages reported)");
    }
    auto* device = manager->GetDevice();
    auto fs = std::make_shared<vfs::RootFileSystem>();
    fs->mount("/donut", shaderPath);
    fs->mount("/tests", testShaderPath);
    auto factory = std::make_shared<ShaderFactory>(device, fs, "/");
    auto common = std::make_shared<CommonRenderPasses>(device, factory);
    auto upload = device->createCommandList();
    upload->open();
    Fixture fixture(device, upload, *common);
    upload->close();
    device->executeCommandList(upload);
    device->waitForIdle();

    bool passed = true;
    for (bool inputAssembler : { false, true })
    {
        TestDepthPassT<DepthPass> depthPass(device, common);
        DepthPass::CreateParameters depthParams;
        depthParams.useInputAssembler = inputAssembler;
        // These adapters only replace the pixel shader; retain the stock input policy.
        depthPass.Init(*factory, depthParams);
        DepthPass::Context depthContext;
        passed &= fixture.ExercisePass(device, depthPass, depthContext,
            inputAssembler ? "Depth IA mixed UV formats" : "Depth raw mixed UV formats", inputAssembler);

        TestForwardPassT<ForwardShadingPass> forwardPass(device, common);
        ForwardShadingPass::CreateParameters forwardParams;
        forwardParams.useInputAssembler = inputAssembler;
        forwardPass.Init(*factory, forwardParams);
        ForwardShadingPass::Context forwardContext;
        passed &= fixture.ExercisePass(device, forwardPass, forwardContext,
            inputAssembler ? "Forward IA mixed UV formats" : "Forward raw mixed UV formats", inputAssembler);

        for (bool motionVectors : { false, true })
        {
            TestGBufferPassT<GBufferFillPass> gbufferPass(device, common);
            GBufferFillPass::CreateParameters gbufferParams;
            gbufferParams.useInputAssembler = inputAssembler;
            gbufferParams.enableMotionVectors = motionVectors;
            gbufferPass.Init(*factory, gbufferParams);
            GBufferFillPass::Context gbufferContext;
            const char* name = inputAssembler
                ? (motionVectors ? "GBuffer IA mixed UV formats + motion vectors" : "GBuffer IA mixed UV formats")
                : (motionVectors ? "GBuffer raw mixed UV formats + motion vectors" : "GBuffer raw mixed UV formats");
            passed &= fixture.ExercisePass(device, gbufferPass, gbufferContext, name, inputAssembler);
        }

        {
            TestDepthLayoutPass genericDepthPass(device, common);
            CustomDepthPass::CreateParameters genericDepthParams;
            genericDepthParams.useInputAssembler = inputAssembler;
            genericDepthPass.Init(*factory, genericDepthParams);
            // Additional layouts are created only when a format is first drawn.
            passed &= genericDepthPass.float16LayoutCount == 0;
            passed &= genericDepthPass.unorm16LayoutCount == 0;
            CustomDepthPass::Context genericDepthContext;
            passed &= fixture.ExercisePass(device, genericDepthPass, genericDepthContext,
                inputAssembler ? "Depth IA custom input policy" : "Depth raw custom input policy");
            passed &= genericDepthPass.float16LayoutCount == (inputAssembler ? 1u : 0u);
            passed &= genericDepthPass.unorm16LayoutCount == (inputAssembler ? 1u : 0u);

            TestForwardPassT<TestInputFactories<CustomForwardShadingPass>> genericForwardPass(device, common);
            CustomForwardShadingPass::CreateParameters genericForwardParams;
            genericForwardParams.useInputAssembler = inputAssembler;
            genericForwardPass.Init(*factory, genericForwardParams);
            CustomForwardShadingPass::Context genericForwardContext;
            passed &= fixture.ExercisePass(device, genericForwardPass, genericForwardContext,
                inputAssembler ? "Forward IA custom input policy" : "Forward raw custom input policy");

            for (bool motionVectors : { false, true })
            {
                TestGBufferPassT<TestInputFactories<CustomGBufferFillPass>> genericGBufferPass(device, common);
                CustomGBufferFillPass::CreateParameters gbufferParams;
                gbufferParams.useInputAssembler = inputAssembler;
                gbufferParams.enableMotionVectors = motionVectors;
                genericGBufferPass.Init(*factory, gbufferParams);
                CustomGBufferFillPass::Context genericGBufferContext;
                const char* name = inputAssembler
                    ? (motionVectors ? "GBuffer IA custom input policy + motion vectors" : "GBuffer IA custom input policy")
                    : (motionVectors ? "GBuffer raw custom input policy + motion vectors" : "GBuffer raw custom input policy");
                passed &= fixture.ExercisePass(device, genericGBufferPass, genericGBufferContext, name);
            }
        }

        // Custom vertex shaders and input factories use the explicit custom base.
        LegacyDepthPass legacyPass(device, common);
        CustomDepthPass::CreateParameters legacyParams;
        legacyParams.useInputAssembler = inputAssembler;
        legacyPass.Init(*factory, legacyParams);
        CustomDepthPass::Context legacyContext;
        const auto unmodifiedExpected = fixture.expected;
        if (inputAssembler)
            for (size_t group = 0; group < GroupCount; ++group)
                fixture.expected[group] += float2(0.125f + 0.0625f * fixture.draws[group].mesh->vertexOffset, 0.25f);
        passed &= fixture.ExercisePass(device, legacyPass, legacyContext,
            inputAssembler ? "Custom legacy depth IA shader" : "Custom legacy depth raw shader");
        passed &= legacyPass.inputBindingCount > 0 && legacyPass.inputBindingsReceivedBuffers;
        fixture.expected = unmodifiedExpected;
    }
    device->waitForIdle();
    return passed;
}
