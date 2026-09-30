/*
 * Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/math/float.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/GltfImporter.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/DepthPass.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/tests/utils.h>
#include <nvrhi/utils.h>
#include <nvrhi/common/misc.h>

using namespace donut::math;
#include <donut/shaders/bindless.h>
#include <donut/shaders/depth_cb.h>
#include <donut/shaders/forward_cb.h>
#include <donut/shaders/gbuffer_cb.h>

#include <cstring>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <atomic>

using namespace donut;
using namespace donut::engine;
using namespace donut::math;

namespace
{
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
}

static_assert(sizeof(GeometryData) == 96);
static_assert(offsetof(GeometryData, texCoordFormat) == 52);
static_assert(offsetof(GeometryData, texCoord1ScaleBias) == 64);
static_assert(offsetof(GeometryData, texCoord2ScaleBias) == 80);
static_assert(sizeof(DepthPushConstants) == 48);
static_assert(sizeof(ForwardPushConstants) == 48);
static_assert(sizeof(GBufferPushConstants) == 48);

template<typename PipelineKey>
static void CheckPipelineKeys()
{
    static_assert(sizeof(PipelineKey) == sizeof(uint32_t));
    bool occupied[PipelineKey::Count] = {};
    // All existing cull/boolean combinations must remain distinct for all three UV formats.
    for (uint32_t flags = 0; flags < 32; ++flags)
    {
        for (auto format : { TexCoordFormat::Float32, TexCoordFormat::Float16, TexCoordFormat::Unorm16 })
        {
            PipelineKey key;
            key.value = flags;
            key.bits.texCoordFormat = uint8_t(format);
            CHECK(key.value < PipelineKey::Count);
            CHECK(!occupied[key.value]);
            occupied[key.value] = true;
        }
    }
}

static void TestDescriptors()
{
    CheckPipelineKeys<donut::render::DepthPass::PipelineKey>();
    CheckPipelineKeys<donut::render::GBufferFillPass::PipelineKey>();
    BufferGroup buffers;
    CHECK(buffers.texCoordFormat == TexCoordFormat::Float32);
    CHECK(buffers.getTexCoordStride() == 8);
    for (VertexAttribute attribute : { VertexAttribute::TexCoord1, VertexAttribute::TexCoord2 })
    {
        auto legacy = GetVertexAttributeDesc(attribute, "TEXCOORD", 2);
        CHECK(legacy.format == nvrhi::Format::RG32_FLOAT && legacy.elementStride == 8);
        auto packed = GetVertexAttributeDesc(attribute, "TEXCOORD", 2, TexCoordFormat::Float16);
        CHECK(packed.format == nvrhi::Format::RG16_FLOAT && packed.elementStride == 4);
        CHECK(packed.bufferIndex == legacy.bufferIndex && packed.arraySize == legacy.arraySize);
        auto normalized = GetVertexAttributeDesc(attribute, "TEXCOORD", 2, TexCoordFormat::Unorm16);
        CHECK(normalized.format == nvrhi::Format::RG16_UNORM && normalized.elementStride == 4);
        CHECK(normalized.bufferIndex == legacy.bufferIndex && normalized.arraySize == legacy.arraySize);
    }
    buffers.texCoordFormat = TexCoordFormat::Float16;
    CHECK(buffers.getTexCoordStride() == 4);
    auto position = GetVertexAttributeDesc(VertexAttribute::Position, "POSITION", 0, buffers.texCoordFormat);
    CHECK(position.format == nvrhi::Format::RGB32_FLOAT && position.elementStride == 12);
    buffers.texCoordFormat = TexCoordFormat::Unorm16;
    CHECK(buffers.getTexCoordStride() == 4);
}

static void TestImporterStoragePolicy()
{
    class ImportFileSystem : public vfs::NativeFileSystem
    {
    public:
        std::shared_ptr<vfs::IBlob> readFile(const std::filesystem::path& name) override
        {
            static const char gltf[] = R"({
                "asset":{"version":"2.0"},
                "buffers":[{"uri":"uv.bin","byteLength":60}],
                "bufferViews":[{"buffer":0,"byteLength":36},
                    {"buffer":0,"byteOffset":36,"byteLength":24}],
                "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
                    "min":[0,0,0],"max":[1,1,0]},
                    {"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],
                "materials":[{}],
                "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],
                "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0
            })";
            static const float vertices[] = {
                0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f,
                -2.25f, 3.5f, 3000.125f, -7.f, 0.1234567f, 2.25f
            };
            const bool json = name.filename() == "uv.gltf";
            if (!json && name.filename() != "uv.bin")
                return nullptr;
            const void* data = json ? static_cast<const void*>(gltf) : vertices;
            const size_t size = json ? sizeof(gltf) - 1 : sizeof(vertices);
            void* copy = std::malloc(size);
            CHECK(copy != nullptr);
            std::memcpy(copy, data, size);
            return std::make_shared<vfs::Blob>(copy, size);
        }
    };
    class ImportFactory : public SceneTypeFactory
    {
    public:
        std::shared_ptr<MeshInfo> mesh;
        std::shared_ptr<MeshInfo> CreateMesh() override
        {
            return mesh = SceneTypeFactory::CreateMesh();
        }
    };

    auto fs = std::make_shared<ImportFileSystem>();
    auto factory = std::make_shared<ImportFactory>();
    TextureCache textures(nullptr, fs, nullptr);
    SceneLoadingStats stats{};
    const float2 expected[] = { float2(-2.25f, 3.5f), float2(3000.125f, -7.f), float2(0.1234567f, 2.25f) };
    for (TexCoordFormat format : { TexCoordFormat::Float32, TexCoordFormat::Float16, TexCoordFormat::Unorm16 })
    {
        // Exercise the default constructor argument as well as both storage overrides.
        const GltfImporter importer = format == TexCoordFormat::Float32
            ? GltfImporter(fs, factory) : GltfImporter(fs, factory, format);
        factory->mesh.reset();
        SceneImportResult result;
        CHECK(importer.Load("uv.gltf", textures, stats, nullptr, result));
        CHECK(result.rootNode != nullptr && factory->mesh != nullptr);
        const auto& buffers = *factory->mesh->buffers;
        CHECK(buffers.texCoordFormat == format);
        CHECK(buffers.vertexBuffer == nullptr && buffers.texcoord1Data.size() == 3);
        // Storage selection must not quantize, normalize, or otherwise alter the imported CPU UVs.
        for (size_t vertex = 0; vertex < 3; ++vertex)
        {
            CHECK(buffers.texcoord1Data[vertex].x == expected[vertex].x);
            CHECK(buffers.texcoord1Data[vertex].y == expected[vertex].y);
        }
    }
}

static void TestDecodeRangeHints()
{
    BufferGroup buffers;
    TexCoordDecodeRange first;
    first.vertexOffset = 4;
    first.numVertices = 3;
    first.texCoord1.scale = float2(2.f, 3.f);
    first.texCoord1.offset = float2(-8.f, 12.f);
    TexCoordDecodeRange second = first;
    second.vertexOffset = 10;
    second.numVertices = 4;
    second.texCoord1.offset = float2(100.f, -200.f);
    buffers.texCoordDecodeRanges = { first, second };

    // Hints are an optimization: missing, stale, and out-of-range values must
    // preserve the lookup result, including gaps and both interval boundaries.
    for (uint32_t vertex : { 0u, 3u, 4u, 6u, 7u, 9u, 10u, 13u, 14u, ~0u })
    {
        const uint32_t expectedIndex = vertex >= 4 && vertex < 7 ? 0u
            : vertex >= 10 && vertex < 14 ? 1u : ~0u;
        CHECK(buffers.getTexCoordDecodeRangeIndex(vertex) == expectedIndex);
        const auto& expected = buffers.getTexCoordDecodeRange(vertex);
        for (uint32_t hint : { 0u, 1u, 2u, 999u, ~0u })
            CHECK(&buffers.getTexCoordDecodeRange(vertex, hint) == &expected);
    }

    const uint32_t savedHint = buffers.getTexCoordDecodeRangeIndex(11);
    buffers.texCoordDecodeRanges[1].texCoord1.scale = float2(0.f, 7.f);
    buffers.texCoordDecodeRanges[1].texCoord1.offset = float2(-3.f, 0.5f);
    CHECK(buffers.getTexCoordDecodeRange(11, savedHint).texCoord1.scale.y == 7.f);
    CHECK(buffers.getTexCoordDecodeRange(11, savedHint).texCoord1.offset.x == -3.f);

    // A rebuilt vector can move the same domain to another index. A stale hint
    // must search the live vector instead of retaining a pointer or decode copy.
    TexCoordDecodeRange leading = first;
    leading.vertexOffset = 0;
    leading.numVertices = 2;
    std::vector<TexCoordDecodeRange> rebuilt = { leading, first, second };
    rebuilt[2].texCoord2.scale = float2(9.f, 11.f);
    buffers.texCoordDecodeRanges.swap(rebuilt);
    CHECK(buffers.getTexCoordDecodeRangeIndex(11) == 2u);
    CHECK(&buffers.getTexCoordDecodeRange(11, savedHint) == &buffers.texCoordDecodeRanges[2]);
    CHECK(buffers.getTexCoordDecodeRange(11, savedHint).texCoord2.scale.y == 11.f);

    buffers.texCoordDecodeRanges.clear();
    CHECK(buffers.getTexCoordDecodeRangeIndex(11) == ~0u);
    const auto& identity = buffers.getTexCoordDecodeRange(11, savedHint);
    CHECK(identity.texCoord1.scale.x == 1.f && identity.texCoord1.scale.y == 1.f);
    CHECK(identity.texCoord1.offset.x == 0.f && identity.texCoord1.offset.y == 0.f);

    // Unsigned subtraction alone would mistake a vertex before this interval
    // for a hit when its length is near the uint32 limit.
    second.vertexOffset = 10;
    second.numVertices = ~0u;
    buffers.texCoordDecodeRanges = { second };
    CHECK(buffers.getTexCoordDecodeRangeIndex(8) == ~0u);
    CHECK(&buffers.getTexCoordDecodeRange(8, 0) == &buffers.getTexCoordDecodeRange(8));
}

class TestScene : public Scene
{
public:
    using Scene::Scene;
    // Exercise geometry metadata without requiring a bindless descriptor table on DX11.
    void EnableGeometryMetadata() { m_EnableBindlessResources = true; }
};

struct Fixture
{
    std::shared_ptr<MeshInfo> mesh;
    std::shared_ptr<SkinnedMeshInstance> skin;
    std::vector<float2> uv1;
    std::vector<float2> uv2;
    TexCoordFormat expectedFormat;
};

static Fixture AddFixture(const std::shared_ptr<SceneGraph>& graph,
    const std::shared_ptr<SceneTypeFactory>& factory, TexCoordFormat format, uint32_t count,
    bool secondStream, bool skinned, float exceptional = 0.f)
{
    Fixture fixture;
    fixture.expectedFormat = exceptional == 0.f ? format : TexCoordFormat::Float32;
    fixture.mesh = factory->CreateMesh();
    auto& mesh = fixture.mesh;
    mesh->name = "UV test";
    mesh->buffers = std::make_shared<BufferGroup>();
    auto& buffers = *mesh->buffers;
    buffers.texCoordFormat = format;
    const uint32_t paddedCount = (count + 3u) & ~3u;
    buffers.positionData.resize(paddedCount, float3(0.f));
    buffers.indexData = { 0, 0, 0 };
    for (uint32_t i = 0; i < count; ++i)
    {
        fixture.uv1.push_back(format == TexCoordFormat::Unorm16
            ? float2(3000.f + float(i) / 7.f, -12.5f) // Large offset and a constant axis.
            : float2(float(i) / 7.f, -float(i) - 0.25f));
        if (secondStream)
            fixture.uv2.push_back(format == TexCoordFormat::Unorm16
                ? float2(-40.f + float(i) * 3.25f, float(i) / 3.f)
                : float2(2.5f + float(i), float(i) / 3.f));
    }
    if (exceptional != 0.f)
        (secondStream ? fixture.uv2 : fixture.uv1).back().y = exceptional;
    buffers.texcoord1Data = fixture.uv1;
    buffers.texcoord2Data = fixture.uv2;

    mesh->vertexOffset = count >= 7 ? 1 : 0;
    mesh->totalVertices = count - mesh->vertexOffset;
    mesh->totalIndices = 3;
    auto geometry = factory->CreateMeshGeometry();
    geometry->material = factory->CreateMaterial();
    geometry->vertexOffsetInMesh = count >= 7 ? 2 : 0;
    geometry->numVertices = mesh->totalVertices - geometry->vertexOffsetInMesh;
    geometry->numIndices = 3;
    mesh->geometries.push_back(geometry);

    if (skinned)
    {
        buffers.jointData.resize(paddedCount, vector<uint16_t, 4>(uint16_t(0)));
        buffers.weightData.resize(paddedCount, float4(1.f, 0.f, 0.f, 0.f));
        mesh->isSkinPrototype = true;
        fixture.skin = factory->CreateSkinnedMeshInstance(factory, mesh);
        fixture.skin->joints.push_back({ graph->GetRootNode(), float4x4::identity() });
        graph->AttachLeafNode(graph->GetRootNode(), fixture.skin);
    }
    else
    {
        graph->AttachLeafNode(graph->GetRootNode(), factory->CreateMeshInstance(mesh));
        // A second instance shares this upload and exercises the released CPU arrays.
        graph->AttachLeafNode(graph->GetRootNode(), factory->CreateMeshInstance(mesh));
    }
    return fixture;
}

static void AddSharedFixtures(std::vector<Fixture>& fixtures, const std::shared_ptr<SceneGraph>& graph,
    const std::shared_ptr<SceneTypeFactory>& factory, bool overlap)
{
    Fixture first = AddFixture(graph, factory, TexCoordFormat::Unorm16, 8, true, false);
    first.mesh->vertexOffset = 0;
    first.mesh->totalVertices = overlap ? 5 : 4;
    first.mesh->geometries.front()->vertexOffsetInMesh = 0;
    first.mesh->geometries.front()->numVertices = first.mesh->totalVertices;
    for (uint32_t vertex = 0; vertex < 8; ++vertex)
    {
        first.uv1[vertex] = float2(vertex < 4 ? 3000.f + float(vertex) / 7.f : -100.f + float(vertex), 2.f);
        first.uv2[vertex] = float2(float(vertex) * 2.f, vertex < 4 ? -8.f : 20.f + float(vertex));
    }
    first.mesh->buffers->texcoord1Data = first.uv1;
    first.mesh->buffers->texcoord2Data = first.uv2;

    Fixture second = first;
    second.mesh = factory->CreateMesh();
    second.mesh->buffers = first.mesh->buffers;
    second.mesh->vertexOffset = overlap ? 3 : 4;
    second.mesh->totalVertices = 8 - second.mesh->vertexOffset;
    second.mesh->totalIndices = 3;
    auto geometry = factory->CreateMeshGeometry();
    geometry->material = first.mesh->geometries.front()->material;
    geometry->numVertices = second.mesh->totalVertices;
    geometry->numIndices = 3;
    second.mesh->geometries.push_back(geometry);
    graph->AttachLeafNode(graph->GetRootNode(), factory->CreateMeshInstance(second.mesh));
    fixtures.push_back(first);
    fixtures.push_back(second);
}

static void CheckDecodeRanges(const BufferGroup& buffers, const Fixture& fixture)
{
    if (buffers.texCoordFormat != TexCoordFormat::Unorm16)
        return;
    CHECK(!buffers.texCoordDecodeRanges.empty());
    for (const auto& range : buffers.texCoordDecodeRanges)
    {
        CHECK(range.numVertices > 0);
        for (uint32_t stream = 0; stream < 2; ++stream)
        {
            const auto& uvs = stream == 0 ? fixture.uv1 : fixture.uv2;
            const auto& decode = stream == 0 ? range.texCoord1 : range.texCoord2;
            if (uvs.empty())
                continue;
            float2 minimum = uvs[range.vertexOffset];
            float2 maximum = minimum;
            for (uint32_t vertex = range.vertexOffset; vertex < range.vertexOffset + range.numVertices; ++vertex)
            {
                CHECK(vertex < uvs.size());
                minimum = donut::math::min(minimum, uvs[vertex]);
                maximum = donut::math::max(maximum, uvs[vertex]);
            }
            CHECK(decode.offset.x == minimum.x && decode.offset.y == minimum.y);
            CHECK(decode.scale.x == float(double(maximum.x) - minimum.x));
            CHECK(decode.scale.y == float(double(maximum.y) - minimum.y));
        }
    }
}

static void CheckGeometry(TestScene& scene, const MeshInfo& mesh, TexCoordFormat format)
{
    const auto& buffers = *mesh.buffers;
    CHECK(buffers.texCoordFormat == format);
    for (const auto& geometry : mesh.geometries)
    {
        const GeometryData* data = scene.GetGeometryData(*geometry);
        CHECK(data != nullptr && data->texCoordFormat == uint32_t(format));
        const uint32_t vertex = mesh.vertexOffset + geometry->vertexOffsetInMesh;
        CHECK(data->texCoord1Offset == buffers.getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset
            + vertex * buffers.getTexCoordStride());
        CHECK(data->texCoord2Offset == (buffers.hasAttribute(VertexAttribute::TexCoord2)
            ? buffers.getVertexBufferRange(VertexAttribute::TexCoord2).byteOffset + vertex * buffers.getTexCoordStride()
            : ~0u));
        const auto& decode = buffers.getTexCoordDecodeRange(vertex);
        CHECK(geometry->texCoordDecodeRangeIndex == buffers.getTexCoordDecodeRangeIndex(vertex));
        CHECK(&buffers.getTexCoordDecodeRange(vertex, geometry->texCoordDecodeRangeIndex) == &decode);
        const float4 scaleBias1(decode.texCoord1.scale, decode.texCoord1.offset);
        const float4 scaleBias2(decode.texCoord2.scale, decode.texCoord2.offset);
        CHECK(std::memcmp(&data->texCoord1ScaleBias, &scaleBias1, sizeof(scaleBias1)) == 0);
        CHECK(std::memcmp(&data->texCoord2ScaleBias, &scaleBias2, sizeof(scaleBias2)) == 0);
    }
}

static void CheckGpuUVs(nvrhi::IDevice* device, nvrhi::IComputePipeline* pipeline,
    nvrhi::IBindingLayout* layout, const BufferGroup& buffers, const Fixture& fixture, bool skinned)
{
    const uint32_t first = skinned ? fixture.mesh->vertexOffset : 0;
    const uint32_t count = uint32_t(fixture.uv1.size()) - first;
    const auto& range1 = buffers.getVertexBufferRange(VertexAttribute::TexCoord1);
    const auto& range2 = buffers.getVertexBufferRange(VertexAttribute::TexCoord2);
    CHECK(range1.byteSize == nvrhi::align(uint64_t(count) * buffers.getTexCoordStride(), uint64_t(16)));
    CHECK(range1.byteOffset % 16 == 0 && range2.byteOffset % 16 == 0);
    CHECK(range2.byteSize == (fixture.uv2.empty() ? 0 : range1.byteSize));

    auto output = device->createBuffer(nvrhi::BufferDesc().setByteSize(count * 32)
        .setCanHaveRawViews(true).setCanHaveUAVs(true).setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true));
    auto readback = device->createBuffer(nvrhi::BufferDesc().setByteSize(count * 32)
        .setCpuAccess(nvrhi::CpuAccessMode::Read).setInitialState(nvrhi::ResourceStates::CopyDest));
    std::vector<float4> scaleBias(count * 2);
    for (uint32_t vertex = 0; vertex < count; ++vertex)
    {
        const auto& decode = buffers.getTexCoordDecodeRange(vertex);
        scaleBias[vertex * 2] = float4(decode.texCoord1.scale, decode.texCoord1.offset);
        scaleBias[vertex * 2 + 1] = float4(decode.texCoord2.scale, decode.texCoord2.offset);
    }
    auto decodeBuffer = device->createBuffer(nvrhi::BufferDesc().setByteSize(scaleBias.size() * sizeof(float4))
        .setCanHaveRawViews(true).setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
    auto bindings = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::PushConstants(0, 16))
        .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(0, buffers.vertexBuffer))
        .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(1, decodeBuffer))
        .addItem(nvrhi::BindingSetItem::RawBuffer_UAV(0, output)), layout);
    uint32_t constants[] = { count, uint32_t(buffers.texCoordFormat), uint32_t(range1.byteOffset),
        fixture.uv2.empty() ? ~0u : uint32_t(range2.byteOffset) };
    auto commands = device->createCommandList();
    commands->open();
    commands->writeBuffer(decodeBuffer, scaleBias.data(), scaleBias.size() * sizeof(float4));
    nvrhi::ComputeState state;
    state.pipeline = pipeline;
    state.bindings = { bindings };
    commands->setComputeState(state);
    commands->setPushConstants(constants, sizeof(constants));
    commands->dispatch((count + 63) / 64);
    commands->copyBuffer(readback, 0, output, 0, count * 32);
    commands->close();
    device->executeCommandList(commands);
    device->waitForIdle();
    const auto* result = static_cast<const uint32_t*>(device->mapBuffer(readback, nvrhi::CpuAccessMode::Read));
    CHECK(result != nullptr);
    bool matches = true;
    for (uint32_t vertex = 0; vertex < count; ++vertex)
    {
        for (uint32_t stream = 0; stream < 2; ++stream)
        {
            uint32_t expected[4] = {};
            const auto& uvs = stream == 0 ? fixture.uv1 : fixture.uv2;
            if (!uvs.empty())
            {
                float2 decoded = uvs[first + vertex];
                if (buffers.texCoordFormat == TexCoordFormat::Unorm16)
                {
                    float2 actual;
                    std::memcpy(&actual, result + vertex * 8 + stream * 4, sizeof(actual));
                    const auto& decode = buffers.getTexCoordDecodeRange(vertex);
                    const auto& transform = stream == 0 ? decode.texCoord1 : decode.texCoord2;
                    bool decodedMatches = true;
                    for (uint32_t axis = 0; axis < 2; ++axis)
                    {
                        // Quantization plus FP32 unpack/reconstruction rounding, including large offsets.
                        const double tolerance = double(transform.scale[axis]) / (2.0 * 65535.0)
                            + 2.0 * std::numeric_limits<float>::epsilon()
                                * (std::abs(double(transform.offset[axis])) + transform.scale[axis]);
                        decodedMatches &= std::isfinite(actual[axis])
                            && std::abs(double(actual[axis]) - decoded[axis]) <= tolerance;
                    }
                    decodedMatches &= result[vertex * 8 + stream * 4 + 3] == 0;
                    if (!decodedMatches)
                    {
                        const auto* words = result + vertex * 8 + stream * 4;
                        std::fprintf(stderr, "UV mismatch format=%u skinned=%d first=%u count=%u vertex=%u stream=%u: "
                            "expected=(%.9g, %.9g) actual=(%.9g, %.9g) scale=(%.9g, %.9g) bias=(%.9g, %.9g) "
                            "words=(%08x, %08x, %08x, %08x)\n",
                            uint32_t(buffers.texCoordFormat), int(skinned), first, count, vertex, stream,
                            decoded.x, decoded.y, actual.x, actual.y, transform.scale.x, transform.scale.y,
                            transform.offset.x, transform.offset.y, words[0], words[1], words[2], words[3]);
                    }
                    matches &= decodedMatches;
                    continue;
                }
                else if (buffers.texCoordFormat == TexCoordFormat::Float16)
                {
                    const auto packed = Float32ToFloat16x2(decoded);
                    expected[2] = packed.bits;
                    decoded = Float16ToFloat32x2(packed);
                }
                else
                    std::memcpy(expected + 2, &decoded, sizeof(decoded));
                std::memcpy(expected, &decoded, sizeof(decoded));
            }
            const auto* actual = result + vertex * 8 + stream * 4;
            const bool wordsMatch = std::memcmp(actual, expected, sizeof(expected)) == 0;
            if (!wordsMatch)
            {
                std::fprintf(stderr, "UV mismatch format=%u skinned=%d first=%u count=%u vertex=%u stream=%u: "
                    "expected words=(%08x, %08x, %08x, %08x) actual=(%08x, %08x, %08x, %08x)\n",
                    uint32_t(buffers.texCoordFormat), int(skinned), first, count, vertex, stream,
                    expected[0], expected[1], expected[2], expected[3], actual[0], actual[1], actual[2], actual[3]);
            }
            matches &= wordsMatch;
        }
    }
    device->unmapBuffer(readback);
    CHECK(matches);
}

static void TestGpu(nvrhi::IDevice* device, const char* shaderDirectory)
{
    auto fs = std::make_shared<vfs::RootFileSystem>();
    fs->mount("/donut", shaderDirectory);
    fs->mount("/tests", std::filesystem::path(DONUT_TEST_SHADER_DIR)
        / (device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11 ? "dxbc" :
            device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 ? "dxil" : "spirv"));
    ShaderFactory shaders(device, fs, "/");
    auto shader = shaders.CreateShader("tests/texcoords_cs", "main", nullptr, nvrhi::ShaderType::Compute);
    CHECK(shader != nullptr);
    auto layout = device->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, 16))
        .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::RawBuffer_UAV(0)));
    nvrhi::ComputePipelineDesc desc;
    desc.CS = shader;
    desc.bindingLayouts = { layout };
    auto pipeline = device->createComputePipeline(desc);
    CHECK(pipeline != nullptr);

    auto factory = std::make_shared<SceneTypeFactory>();
    TestScene scene(device, shaders, fs, nullptr, nullptr, factory);
    const bool geometryMetadata = device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11;
    if (geometryMetadata)
        scene.EnableGeometryMetadata();
    auto graph = scene.CreateSceneGraph();
    graph->SetRootNode(std::make_shared<SceneGraphNode>());
    std::vector<Fixture> fixtures;
    for (auto format : { TexCoordFormat::Float32, TexCoordFormat::Float16, TexCoordFormat::Unorm16 })
    {
        for (uint32_t count : { 1u, 2u, 3u, 7u })
            fixtures.push_back(AddFixture(graph, factory, format, count, true, false));
        fixtures.push_back(AddFixture(graph, factory, format, 7, false, false));
        fixtures.push_back(AddFixture(graph, factory, format, 7, true, true));
    }
    for (float exceptional : { 65505.f, -65505.f, std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN() })
        fixtures.push_back(AddFixture(graph, factory, TexCoordFormat::Float16, 7, true, false, exceptional));
    for (float exceptional : { std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN() })
        fixtures.push_back(AddFixture(graph, factory, TexCoordFormat::Unorm16, 7, true, false, exceptional));
    Fixture overflow = AddFixture(graph, factory, TexCoordFormat::Unorm16, 7, true, false);
    overflow.uv2[1].x = -std::numeric_limits<float>::max();
    overflow.uv2.back().x = std::numeric_limits<float>::max();
    overflow.mesh->buffers->texcoord2Data = overflow.uv2;
    overflow.expectedFormat = TexCoordFormat::Float32;
    fixtures.push_back(overflow);
    const size_t disjointFixture = fixtures.size();
    AddSharedFixtures(fixtures, graph, factory, false);
    const size_t overlappingFixture = fixtures.size();
    AddSharedFixtures(fixtures, graph, factory, true);

    scene.FinishedLoading(0);
    CHECK(fixtures[disjointFixture].mesh->buffers->texCoordDecodeRanges.size() == 2);
    CHECK(fixtures[overlappingFixture].mesh->buffers->texCoordDecodeRanges.size() == 1);
    for (const auto& fixture : fixtures)
    {
        CHECK(fixture.mesh->buffers->texcoord1Data.empty() && fixture.mesh->buffers->texcoord2Data.empty());
        CHECK(fixture.mesh->buffers->texCoordFormat == fixture.expectedFormat);
        CheckDecodeRanges(*fixture.mesh->buffers, fixture);
        if (geometryMetadata)
            CheckGeometry(scene, *fixture.mesh, fixture.expectedFormat);
        CheckGpuUVs(device, pipeline, layout, *fixture.mesh->buffers, fixture, false);
        if (fixture.skin)
        {
            CHECK(fixture.skin->GetMesh()->buffers->texCoordFormat == fixture.expectedFormat);
            if (geometryMetadata)
                CheckGeometry(scene, *fixture.skin->GetMesh(), fixture.expectedFormat);
            CheckGpuUVs(device, pipeline, layout, *fixture.skin->GetMesh()->buffers, fixture, true);
        }
    }
    auto commands = device->createCommandList();
    commands->open();
    scene.Refresh(commands, 1);
    commands->close();
    device->executeCommandList(commands);
    for (const auto& fixture : fixtures)
        if (fixture.skin)
            CheckGpuUVs(device, pipeline, layout, *fixture.skin->GetMesh()->buffers, fixture, true);
    device->waitForIdle();
}

int main(int argc, char** argv)
{
    log::ConsoleApplicationMode();
    try
    {
        TestDescriptors();
        TestImporterStoragePolicy();
        TestDecodeRangeHints();
        const char* shaderDirectory = nullptr;
        bool debugRuntime = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--gpu") == 0 && i + 1 < argc)
                shaderDirectory = argv[++i];
            else if (std::strcmp(argv[i], "--debug-runtime") == 0)
                debugRuntime = true;
        }
        const auto graphicsApi = app::GetGraphicsAPIFromCommandLine(argc, argv);
        LogErrorCounter logErrors(debugRuntime && graphicsApi == nvrhi::GraphicsAPI::VULKAN);
        if (shaderDirectory)
        {
            std::unique_ptr<app::DeviceManager> manager(app::DeviceManager::Create(graphicsApi));
            CHECK(manager != nullptr);
            app::DeviceCreationParameters params;
            params.enableNvrhiValidationLayer = true;
            params.enableDebugRuntime = debugRuntime;
#if DONUT_WITH_VULKAN
            if (debugRuntime)
                params.ignoredVulkanValidationMessageLocations.clear();
#endif
            CHECK(manager->CreateHeadlessDevice(params));
            if (debugRuntime && graphicsApi == nvrhi::GraphicsAPI::VULKAN)
            {
                CHECK(manager->IsVulkanLayerEnabled("VK_LAYER_KHRONOS_validation"));
                std::puts("Khronos Vulkan validation layer: ENABLED (errors checked, all messages reported)");
            }
            TestGpu(manager->GetDevice(), shaderDirectory);
            manager->GetDevice()->waitForIdle();
        }
        logErrors.Report();
        CHECK(logErrors.errors.load() == 0);
        printf("Texture coordinate tests: PASS (%s)\n", shaderDirectory ? "CPU and GPU"
            : "CPU; use --gpu <Donut shader directory> [-dx11|-dx12|-vk] [--debug-runtime] for GPU tests");
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
