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

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace donut;
using namespace donut::math;
using namespace donut::engine;
using namespace donut::render;
#include <donut/shaders/bindless.h>

namespace
{
    constexpr uint32_t Width = 160;
    constexpr uint32_t Height = 32;
    constexpr size_t GroupCount = 5;

    class LogErrorCounter
    {
        log::Callback previous = log::GetCallback();
        bool strictVulkanValidation;

    public:
        std::atomic<uint32_t> errors{ 0 };
        std::atomic<uint32_t> nativeWarnings{ 0 };

        explicit LogErrorCounter(bool strictVulkanValidation)
            : strictVulkanValidation(strictVulkanValidation)
        {
            log::SetCallback([this](log::Severity severity, const char* message)
            {
                if (severity >= log::Severity::Error)
                    ++errors;
                else if (severity == log::Severity::Warning && std::strncmp(message, "[Vulkan:", 8) == 0)
                    ++nativeWarnings;
                previous(severity, message);
            });
        }

        ~LogErrorCounter() { log::SetCallback(previous); }

        void Report() const
        {
            if (strictVulkanValidation)
                std::printf("Validation log: %u errors, %u native warnings (all messages reported)\n",
                    errors.load(), nativeWarnings.load());
        }
    };

    nvrhi::ShaderHandle CreateTestPixelShader(ShaderFactory& factory, const char* entry = "scene")
    {
        return factory.CreateShader("tests/texcoord_raster.hlsl", entry, nullptr, nvrhi::ShaderType::Pixel);
    }

    class TestDepthPass : public DepthPass
    {
    public:
        using DepthPass::DepthPass;
        void ClearMaterialBindings() { m_MaterialBindings->Clear(); }
        uint32_t float16LayoutCount = 0;
        uint32_t unorm16LayoutCount = 0;

    protected:
        nvrhi::ShaderHandle CreatePixelShader(ShaderFactory& factory, const CreateParameters&) override
        {
            return CreateTestPixelShader(factory, "depth");
        }

        nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* shader, const CreateParameters& params, TexCoordFormat format) override
        {
            if (format == TexCoordFormat::Float16)
                ++float16LayoutCount;
            if (format == TexCoordFormat::Unorm16)
                ++unorm16LayoutCount;
            return DepthPass::CreateInputLayout(shader, params, format);
        }
    };

    class TestForwardPass : public ForwardShadingPass
    {
    public:
        using ForwardShadingPass::ForwardShadingPass;
        void ClearMaterialBindings() { m_MaterialBindings->Clear(); }

    protected:
        nvrhi::ShaderHandle CreatePixelShader(ShaderFactory& factory, const CreateParameters&, bool) override
        {
            return CreateTestPixelShader(factory);
        }
    };

    class TestGBufferPass : public GBufferFillPass
    {
    public:
        using GBufferFillPass::GBufferFillPass;
        void ClearMaterialBindings() { m_MaterialBindings->Clear(); }

    protected:
        nvrhi::ShaderHandle CreatePixelShader(ShaderFactory& factory, const CreateParameters&, bool) override
        {
            return CreateTestPixelShader(factory);
        }
    };

    // Existing applications can override the vertex shader. The stock fast path
    // must not silently replace their shader or omit its required constants.
    class LegacyDepthPass : public DepthPass
    {
    public:
        using DepthPass::DepthPass;
        void ClearMaterialBindings() { m_MaterialBindings->Clear(); }

    protected:
        nvrhi::ShaderHandle CreateVertexShader(ShaderFactory& factory, const CreateParameters& params) override
        {
            return params.useInputAssembler
                ? factory.CreateShader("tests/texcoord_raster.hlsl", "custom_depth", nullptr, nvrhi::ShaderType::Vertex)
                : DepthPass::CreateVertexShader(factory, params);
        }

        nvrhi::ShaderHandle CreatePixelShader(ShaderFactory& factory, const CreateParameters&) override
        {
            return CreateTestPixelShader(factory, "depth");
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
            ForwardShadingPass* forward = nullptr, bool stressStateChanges = false, bool manualDraws = false,
            uint32_t viewRepetitions = 1, const std::function<void()>& resetBindings = {})
        {
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
            if (forward)
                forward->PrepareLights(static_cast<ForwardShadingPass::Context&>(context), commands, {}, float3(0.f), float3(0.f), {});
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
            bool passResult = true;
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
            ForwardShadingPass* forward = nullptr)
        {
            bool passed = RenderAndCheck(device, pass, context, name, forward);
            // Reuse the pass and context across command lists, and twice in one list.
            passed &= RenderAndCheck(device, pass, context, (std::string(name) + " repeated/state resets").c_str(),
                forward, true, false, 2);

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
                forward, true);
            ranges = savedRanges;
            expected = savedExpected;
            passed &= RenderAndCheck(device, pass, context, (std::string(name) + " manual API").c_str(),
                forward, true, true, 2);
            // Clear the binding cache inside one RenderView, between two FP32
            // buffers with the same material and pipeline key. The first draw
            // retains its original CB; each later draw must use the replacement.
            const auto originalMaterialBuffer = material->materialConstants;
            passed &= RenderAndCheck(device, pass, context, (std::string(name) + " material cache clear").c_str(),
                forward, false, false, 1, [&]()
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
        TestDepthPass depthPass(device, common);
        DepthPass::CreateParameters depthParams;
        depthParams.useInputAssembler = inputAssembler;
        // These adapters only replace the pixel shader; the stock vertex path is safe.
        depthParams.specializeInputAssemblerTexCoords = true;
        depthPass.Init(*factory, depthParams);
        passed &= depthPass.float16LayoutCount == 0; // Legacy FP32 layouts must not eagerly require an FP16 layout.
        passed &= depthPass.unorm16LayoutCount == 0;
        DepthPass::Context depthContext;
        passed &= fixture.ExercisePass(device, depthPass, depthContext, inputAssembler ? "Depth IA mixed UV formats" : "Depth raw mixed UV formats");
        passed &= depthPass.float16LayoutCount == (inputAssembler ? 1u : 0u);
        passed &= depthPass.unorm16LayoutCount == (inputAssembler ? 1u : 0u);

        TestForwardPass forwardPass(device, common);
        ForwardShadingPass::CreateParameters forwardParams;
        forwardParams.useInputAssembler = inputAssembler;
        forwardParams.specializeInputAssemblerTexCoords = true;
        forwardPass.Init(*factory, forwardParams);
        ForwardShadingPass::Context forwardContext;
        passed &= fixture.ExercisePass(device, forwardPass, forwardContext, inputAssembler ? "Forward IA mixed UV formats" : "Forward raw mixed UV formats", &forwardPass);

        for (bool motionVectors : { false, true })
        {
            TestGBufferPass gbufferPass(device, common);
            GBufferFillPass::CreateParameters gbufferParams;
            gbufferParams.useInputAssembler = inputAssembler;
            gbufferParams.enableMotionVectors = motionVectors;
            gbufferParams.specializeInputAssemblerTexCoords = true;
            gbufferPass.Init(*factory, gbufferParams);
            GBufferFillPass::Context gbufferContext;
            const char* name = inputAssembler
                ? (motionVectors ? "GBuffer IA mixed UV formats + motion vectors" : "GBuffer IA mixed UV formats")
                : (motionVectors ? "GBuffer raw mixed UV formats + motion vectors" : "GBuffer raw mixed UV formats");
            passed &= fixture.ExercisePass(device, gbufferPass, gbufferContext, name);
        }

        if (inputAssembler)
        {
            TestDepthPass genericDepthPass(device, common);
            depthParams.specializeInputAssemblerTexCoords = false;
            genericDepthPass.Init(*factory, depthParams);
            DepthPass::Context genericDepthContext;
            passed &= fixture.ExercisePass(device, genericDepthPass, genericDepthContext,
                "Depth IA specialization disabled");
        }

        // A custom vertex shader keeps the automatic, unspecialized default.
        depthParams.specializeInputAssemblerTexCoords.reset();
        LegacyDepthPass legacyPass(device, common);
        legacyPass.Init(*factory, depthParams);
        DepthPass::Context legacyContext;
        const auto unmodifiedExpected = fixture.expected;
        if (inputAssembler)
            for (size_t group = 0; group < GroupCount; ++group)
                fixture.expected[group] += float2(0.125f + 0.0625f * fixture.draws[group].mesh->vertexOffset, 0.25f);
        passed &= fixture.ExercisePass(device, legacyPass, legacyContext,
            inputAssembler ? "Custom legacy depth IA shader" : "Custom legacy depth raw shader");
        fixture.expected = unmodifiedExpected;
    }
    device->waitForIdle();
    return passed;
}
