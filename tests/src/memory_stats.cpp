// Headless native-device regression for the public Donut memory statistics APIs.
// No command lists are submitted and no windows or rendering workloads are created.
#include <donut/core/vfs/VFS.h>
#include <donut/engine/MemoryStats.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#if DONUT_WITH_DX11
#include <nvrhi/d3d11.h>
#endif
#include <nvrhi/d3d12.h>
#if TEST_VALIDATION
#include <nvrhi/validation.h>
#endif
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cinttypes>
#include <cstdio>
#include <stdexcept>
#include <string>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace donut::engine;
using Microsoft::WRL::ComPtr;

static void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

struct Messages : nvrhi::IMessageCallback
{
    unsigned errors = 0;
    void message(nvrhi::MessageSeverity severity, const char* text) override
    {
        if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal)
            ++errors;
        std::printf("NVRHI: %s\n", text);
    }
};

static void expectUsage(const GpuMemoryUsage& usage, uint64_t capacity, uint64_t allocation,
    uint32_t count, uint32_t unknown)
{
    check(usage.capacityBytes == capacity, "descriptor capacity total");
    check(usage.allocationBytes == allocation, "backing allocation total");
    check(usage.resourceCount == count, "unique resource count");
    check(usage.unknownAllocationCount == unknown, "unknown allocation count");
    check(usage.hasKnownAllocationBytes() == (unknown == 0), "unknown versus known zero");
}

static void expectEmpty(const SceneGeometryMemoryStats& stats)
{
    expectUsage(stats.vertexBuffers, 0, 0, 0, 0);
    expectUsage(stats.indexBuffers, 0, 0, 0, 0);
}

static uint64_t checkAllocation(nvrhi::IDevice* device, nvrhi::IBuffer* buffer, bool supported)
{
    uint64_t bytes = 123;
    check(tryGetResourceAllocationBytes(device, buffer, bytes) == supported, "allocation availability");
    if (!supported)
    {
        check(bytes == 123, "D3D11 optional query preserves output without legacy assertion");
        return 0;
    }

    ID3D12Device* nativeDevice = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
    ID3D12Resource* nativeBuffer = buffer->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
    check(nativeDevice && nativeBuffer, "native D3D12 objects");
    const auto desc = nativeBuffer->GetDesc();
    const auto expected = nativeDevice->GetResourceAllocationInfo(0, 1, &desc);
    check(bytes == expected.SizeInBytes && bytes > buffer->getDesc().byteSize,
        "allocation requirements match native query, not descriptor capacity");
    std::printf("Buffer capacity=%" PRIu64 " allocation=%" PRIu64 "\n", buffer->getDesc().byteSize, bytes);
    return bytes;
}

static void runScenarios(nvrhi::IDevice* device, const char* shaderPath)
{
    const bool supported = device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12;
    auto a = device->createBuffer(nvrhi::BufferDesc().setByteSize(1024));
    auto b = device->createBuffer(nvrhi::BufferDesc().setByteSize(2048));
    auto c = device->createBuffer(nvrhi::BufferDesc().setByteSize(4096));
    check(a && b && c, "real native buffers");
    const uint64_t aBytes = checkAllocation(device, a, supported);
    const uint64_t bBytes = checkAllocation(device, b, supported);
    const uint64_t cBytes = checkAllocation(device, c, supported);
    std::puts("PASS: real allocation versus descriptor capacity / D3D11 unavailable");

    uint64_t sentinel = 987;
    check(!tryGetResourceAllocationBytes(nullptr, a, sentinel) && sentinel == 987, "null device preserves output");
    check(!tryGetResourceAllocationBytes(device, nullptr, sentinel) && sentinel == 987, "null resource preserves output");
    check(!tryGetResourceAllocationBytes(device, device, sentinel) && sentinel == 987, "unsupported resource preserves output");
    auto texture = device->createTexture(nvrhi::TextureDesc().setWidth(1).setHeight(1).setFormat(nvrhi::Format::RGBA8_UNORM));
    check(texture != nullptr, "create unsupported texture");
    check(!tryGetResourceAllocationBytes(device, texture, sentinel) && sentinel == 987,
        "unsupported texture preserves output without assertion");
    std::puts("PASS: null, non-memory resource and texture output preservation");

    auto fs = std::make_shared<donut::vfs::RootFileSystem>();
    fs->mount("/donut", std::filesystem::path(shaderPath));
    ShaderFactory shaders(device, fs, "/");
    Scene scene(device, shaders, fs, nullptr, nullptr, nullptr);
    expectEmpty(scene.getGeometryMemoryStats()); // No graph yet.
    auto graph = scene.CreateSceneGraph();
    expectEmpty(scene.getGeometryMemoryStats()); // A new graph has no root.
    auto root = std::make_shared<SceneGraphNode>();
    graph->SetRootNode(root);
    expectEmpty(scene.getGeometryMemoryStats());

    auto partial = std::make_shared<MeshInfo>();
    partial->buffers = nullptr;
    auto partialNode = graph->AttachLeafNode(root, std::make_shared<MeshInstance>(partial));
    expectEmpty(scene.getGeometryMemoryStats());
    partial->buffers = std::make_shared<BufferGroup>();
    expectEmpty(scene.getGeometryMemoryStats());
    partial->buffers->vertexBuffer = a;
    auto stats = scene.getGeometryMemoryStats();
    expectUsage(stats.vertexBuffers, 1024, aBytes, 1, supported ? 0 : 1);
    expectUsage(stats.indexBuffers, 0, 0, 0, 0);
    graph->Detach(partialNode);
    expectEmpty(scene.getGeometryMemoryStats());
    std::puts("PASS: absent graph, empty and partially populated scenes");

    auto mesh = std::make_shared<MeshInfo>();
    mesh->buffers = std::make_shared<BufferGroup>();
    mesh->buffers->vertexBuffer = a;
    mesh->buffers->indexBuffer = b;
    auto first = graph->AttachLeafNode(root, std::make_shared<MeshInstance>(mesh));
    auto repeated = graph->AttachLeafNode(root, std::make_shared<MeshInstance>(mesh));
    auto sharedGroup = std::make_shared<MeshInfo>();
    sharedGroup->buffers = mesh->buffers;
    auto shared = graph->AttachLeafNode(root, std::make_shared<MeshInstance>(sharedGroup));
    auto aliases = std::make_shared<MeshInfo>();
    aliases->buffers = std::make_shared<BufferGroup>();
    aliases->buffers->vertexBuffer = a; // Same handle, different BufferGroup.
    aliases->buffers->indexBuffer = a; // Same handle also counts in the index category.
    auto crossCategory = graph->AttachLeafNode(root, std::make_shared<MeshInstance>(aliases));
    stats = scene.getGeometryMemoryStats();
    expectUsage(stats.vertexBuffers, 1024, aBytes, 1, supported ? 0 : 1);
    expectUsage(stats.indexBuffers, 3072, aBytes + bBytes, 2, supported ? 0 : 2);
    auto unknown = getSceneGeometryMemoryStats(nullptr, scene);
    expectUsage(unknown.vertexBuffers, 1024, 0, 1, 1);
    expectUsage(unknown.indexBuffers, 3072, 0, 2, 2);
    std::puts("PASS: shared geometry deduplication, cross-category attribution, unknown versus zero");

    aliases->buffers->vertexBuffer = c;
    stats = scene.getGeometryMemoryStats();
    expectUsage(stats.vertexBuffers, 5120, aBytes + cBytes, 2, supported ? 0 : 2);
    expectUsage(stats.indexBuffers, 3072, aBytes + bBytes, 2, supported ? 0 : 2);
    mesh->buffers->indexBuffer = nullptr;
    stats = scene.getGeometryMemoryStats();
    expectUsage(stats.vertexBuffers, 5120, aBytes + cBytes, 2, supported ? 0 : 2);
    expectUsage(stats.indexBuffers, 1024, aBytes, 1, supported ? 0 : 1);
    graph->Detach(crossCategory);
    stats = scene.getGeometryMemoryStats();
    expectUsage(stats.vertexBuffers, 1024, aBytes, 1, supported ? 0 : 1);
    expectUsage(stats.indexBuffers, 0, 0, 0, 0);
    graph->Detach(first);
    graph->Detach(repeated);
    stats = scene.getGeometryMemoryStats(); // Shared mesh still retains the buffer.
    expectUsage(stats.vertexBuffers, 1024, aBytes, 1, supported ? 0 : 1);
    graph->Detach(shared);
    expectEmpty(scene.getGeometryMemoryStats());
    expectEmpty(getSceneGeometryMemoryStats(nullptr, scene));
    std::puts("PASS: replacement and removal recompute totals without stale state");
}

static void runDevice(nvrhi::IDevice* device, Messages& messages, const char* shaderPath)
{
    check(device != nullptr, "NVRHI device");
    std::puts("Device path: raw");
    runScenarios(device, shaderPath);
#if TEST_VALIDATION
    auto validation = nvrhi::validation::createValidationLayer(device);
    std::puts("Device path: validation");
    runScenarios(validation, shaderPath);
#endif
    device->waitForIdle();
    device->runGarbageCollection();
    check(messages.errors == 0, "no backend or validation errors");
}

int main(int argc, char** argv)
{
    // Keep failures headless too, including a regression into D3D11's legacy assert.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    try
    {
        check(argc == 3, "usage: test_memory_stats d3d11|d3d12 shader-directory");
        Messages messages;
        const std::string backend = argv[1];
#if DONUT_WITH_DX11
        if (backend == "d3d11")
        {
            ComPtr<ID3D11Device> native;
            ComPtr<ID3D11DeviceContext> context;
            check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &native, nullptr, &context)), "D3D11 WARP device");
            nvrhi::d3d11::DeviceDesc desc;
            desc.context = context.Get();
            desc.messageCallback = &messages;
            auto device = nvrhi::d3d11::createDevice(desc);
            runDevice(device, messages, argv[2]);
        }
        else
#endif
        if (backend == "d3d12")
        {
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter> adapter;
            check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
            check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
            ComPtr<ID3D12Device> native;
            check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&native))), "D3D12 WARP device");
            ComPtr<ID3D12CommandQueue> queue;
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            check(SUCCEEDED(native->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))), "D3D12 queue");
            nvrhi::d3d12::DeviceDesc desc;
            desc.pDevice = native.Get();
            desc.pGraphicsCommandQueue = queue.Get();
            desc.errorCB = &messages;
            auto device = nvrhi::d3d12::createDevice(desc);
            runDevice(device, messages, argv[2]);
        }
        else
            throw std::runtime_error("unknown backend");
        std::puts("PASS: all memory statistics scenarios");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
