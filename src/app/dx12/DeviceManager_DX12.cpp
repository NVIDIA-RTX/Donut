/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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

/*
License for glfw

Copyright (c) 2002-2006 Marcus Geelnard

Copyright (c) 2006-2019 Camilla Lowy

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would
   be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not
   be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
   distribution.
*/

#include <string>
#include <algorithm>
#include <vector>

#include <donut/app/DeviceManager.h>
#include <donut/app/DeviceManager_DX12.h>
#include <donut/core/log.h>

#include <Windows.h>
#include <dxgi1_5.h>
#include <dxgidebug.h>

#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

#include <sstream>

#if DONUT_WITH_STREAMLINE
#include <StreamlineIntegration.h>
#endif

using nvrhi::RefCountPtr;

using namespace donut::app;

#define HR_RETURN(hr) if(FAILED(hr)) return false;



static bool IsNvDeviceID(UINT id)
{
    return id == 0x10DE;
}

// Adjust window rect so that it is centred on the given adapter.  Clamps to fit if it's too big.
static bool MoveWindowOntoAdapter(IDXGIAdapter* targetAdapter, RECT& rect)
{
    assert(targetAdapter != NULL);

    HRESULT hres = S_OK;
    unsigned int outputNo = 0;
    while (SUCCEEDED(hres))
    {
        nvrhi::RefCountPtr<IDXGIOutput> pOutput;
        hres = targetAdapter->EnumOutputs(outputNo++, &pOutput);

        if (SUCCEEDED(hres) && pOutput)
        {
            DXGI_OUTPUT_DESC OutputDesc;
            pOutput->GetDesc(&OutputDesc);
            const RECT desktop = OutputDesc.DesktopCoordinates;
            const int centreX = (int)desktop.left + (int)(desktop.right - desktop.left) / 2;
            const int centreY = (int)desktop.top + (int)(desktop.bottom - desktop.top) / 2;
            const int winW = rect.right - rect.left;
            const int winH = rect.bottom - rect.top;
            const int left = centreX - winW / 2;
            const int right = left + winW;
            const int top = centreY - winH / 2;
            const int bottom = top + winH;
            rect.left = std::max(left, (int)desktop.left);
            rect.right = std::min(right, (int)desktop.right);
            rect.bottom = std::min(bottom, (int)desktop.bottom);
            rect.top = std::max(top, (int)desktop.top);

            // If there is more than one output, go with the first found.  Multi-monitor support could go here.
            return true;
        }
    }

    return false;
}

void DeviceManager_DX12::ReportLiveObjects()
{
    nvrhi::RefCountPtr<IDXGIDebug> pDebug;
    DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug));

    if (pDebug)
    {
        DXGI_DEBUG_RLO_FLAGS flags = (DXGI_DEBUG_RLO_FLAGS)(DXGI_DEBUG_RLO_IGNORE_INTERNAL | DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_DETAIL);
        HRESULT hr = pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, flags);
        if (FAILED(hr))
        {
            donut::log::error("ReportLiveObjects failed, HRESULT = 0x%08x", hr);
        }
    }
}

bool DeviceManager_DX12::CreateInstanceInternal()
{
#if DONUT_WITH_STREAMLINE
    StreamlineIntegration::Get().InitializePreDevice(nvrhi::GraphicsAPI::D3D12, m_DeviceParams.streamlineAppId, m_DeviceParams.checkStreamlineSignature, m_DeviceParams.enableStreamlineLog);
#endif

    if (!m_DxgiFactory2)
    {
        HRESULT hres = CreateDXGIFactory2(m_DeviceParams.enableDebugRuntime ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&m_DxgiFactory2));
        if (hres != S_OK)
        {
            donut::log::error("ERROR in CreateDXGIFactory2.\n"
                "For more info, get log from debug D3D runtime: (1) Install DX SDK, and enable Debug D3D from DX Control Panel Utility. (2) Install and start DbgView. (3) Try running the program again.\n");
            return false;
        }
    }

    return true;
}

bool DeviceManager_DX12::EnumerateAdapters(std::vector<AdapterInfo>& outAdapters)
{
    if (!m_DxgiFactory2)
        return false;

    outAdapters.clear();

    while (true)
    {
        RefCountPtr<IDXGIAdapter> adapter;
        HRESULT hr = m_DxgiFactory2->EnumAdapters(uint32_t(outAdapters.size()), &adapter);
        if (FAILED(hr))
            return true;

        DXGI_ADAPTER_DESC desc;
        hr = adapter->GetDesc(&desc);
        if (FAILED(hr))
            return false;

        AdapterInfo adapterInfo;

        adapterInfo.name = GetAdapterName(desc);
        adapterInfo.dxgiAdapter = adapter;
        adapterInfo.vendorID = desc.VendorId;
        adapterInfo.deviceID = desc.DeviceId;
        adapterInfo.dedicatedVideoMemory = desc.DedicatedVideoMemory;

        AdapterInfo::LUID luid;
        static_assert(luid.size() == sizeof(desc.AdapterLuid));
        memcpy(luid.data(), &desc.AdapterLuid, luid.size());
        adapterInfo.luid = luid;

        outAdapters.push_back(std::move(adapterInfo));
    }
}

bool DeviceManager_DX12::CreateDevice()
{
    if (m_DeviceParams.enableDebugRuntime)
    {
        RefCountPtr<ID3D12Debug> pDebug;
        HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&pDebug));

        if (SUCCEEDED(hr))
            pDebug->EnableDebugLayer();
        else
            donut::log::warning("Cannot enable DX12 debug runtime, ID3D12Debug is not available.");
    }

    if (m_DeviceParams.enableGPUValidation)
    {
        RefCountPtr<ID3D12Debug3> debugController3;
        HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController3));

        if (SUCCEEDED(hr))
            debugController3->SetEnableGPUBasedValidation(true);
        else
            donut::log::warning("Cannot enable GPU-based validation, ID3D12Debug3 is not available.");
    }
    
    int adapterIndex = m_DeviceParams.adapterIndex;

#if DONUT_WITH_STREAMLINE
    // Auto select best adapter for streamline features
    if (adapterIndex < 0)
        adapterIndex = StreamlineIntegration::Get().FindBestAdapterDX();
#endif

    if (adapterIndex < 0)
        adapterIndex = 0;

    if (FAILED(m_DxgiFactory2->EnumAdapters(adapterIndex, &m_DxgiAdapter)))
    {
        if (adapterIndex == 0)
            donut::log::error("Cannot find any DXGI adapters in the system.");
        else
            donut::log::error("The specified DXGI adapter %d does not exist.", adapterIndex);
            
        return false;
    }

    {
        DXGI_ADAPTER_DESC aDesc;
        m_DxgiAdapter->GetDesc(&aDesc);
        
        m_RendererString = GetAdapterName(aDesc);
        m_IsNvidia = IsNvDeviceID(aDesc.VendorId);
    }

    
    HRESULT hr = D3D12CreateDevice(
        m_DxgiAdapter,
        m_DeviceParams.featureLevel,
        IID_PPV_ARGS(&m_Device12));

    if (FAILED(hr))
    {
        donut::log::error("D3D12CreateDevice failed, error code = 0x%08x", hr);
        return false;
    }
#if DONUT_WITH_STREAMLINE
    StreamlineIntegration::Get().SetD3DDevice(m_Device12);
#endif

    if (m_DeviceParams.enableDebugRuntime)
    {
        RefCountPtr<ID3D12InfoQueue> pInfoQueue;
        m_Device12->QueryInterface(&pInfoQueue);

        if (pInfoQueue)
        {
#ifdef _DEBUG
            if (m_DeviceParams.enableWarningsAsErrors)
                pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
#endif

            D3D12_MESSAGE_ID disableMessageIDs[] = {
                D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_COMMAND_LIST_STATIC_DESCRIPTOR_RESOURCE_DIMENSION_MISMATCH, // descriptor validation doesn't understand acceleration structures
                D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED, // NGX currently generates benign resource creation warnings
            };

            D3D12_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.pIDList = disableMessageIDs;
            filter.DenyList.NumIDs = sizeof(disableMessageIDs) / sizeof(disableMessageIDs[0]);
            pInfoQueue->AddStorageFilterEntries(&filter);
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc;
    ZeroMemory(&queueDesc, sizeof(queueDesc));
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.NodeMask = 1;
    hr = m_Device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_GraphicsQueue));
    HR_RETURN(hr)
    m_GraphicsQueue->SetName(L"Graphics Queue");

    if (m_DeviceParams.enableComputeQueue)
    {
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        hr = m_Device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_ComputeQueue));
        HR_RETURN(hr)
        m_ComputeQueue->SetName(L"Compute Queue");
    }

    if (m_DeviceParams.enableCopyQueue)
    {
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        hr = m_Device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CopyQueue));
        HR_RETURN(hr)
        m_CopyQueue->SetName(L"Copy Queue");
    }

    nvrhi::d3d12::DeviceDesc deviceDesc;
    deviceDesc.errorCB = m_DeviceParams.messageCallback ? m_DeviceParams.messageCallback : &DefaultMessageCallback::GetInstance();
    deviceDesc.pDevice = m_Device12;
    deviceDesc.pGraphicsCommandQueue = m_GraphicsQueue;
    deviceDesc.pComputeCommandQueue = m_ComputeQueue;
    deviceDesc.pCopyCommandQueue = m_CopyQueue;
#if DONUT_WITH_AFTERMATH
    deviceDesc.aftermathEnabled = m_DeviceParams.enableAftermath;
#endif
    deviceDesc.logBufferLifetime = m_DeviceParams.logBufferLifetime;
    deviceDesc.enableHeapDirectlyIndexed = m_DeviceParams.enableHeapDirectlyIndexed;
    deviceDesc.enableRayTracingValidation = m_DeviceParams.enableRayTracingValidation;

    m_NvrhiDevice = nvrhi::d3d12::createDevice(deviceDesc);

    if (m_DeviceParams.enableNvrhiValidationLayer)
    {
        m_NvrhiDevice = nvrhi::validation::createValidationLayer(m_NvrhiDevice);
    }

#if DONUT_WITH_STREAMLINE
    StreamlineIntegration::Get().InitializeDeviceDX(m_NvrhiDevice);
#endif

    return true;
}

bool DeviceManager_DX12::CreateSwapChain()
{
    UINT windowStyle = m_DeviceParams.startFullscreen
        ? (WS_POPUP | WS_SYSMENU | WS_VISIBLE)
        : m_DeviceParams.startMaximized
            ? (WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_MAXIMIZE)
            : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);

    RECT rect = { 0, 0, LONG(m_DeviceParams.backBufferWidth), LONG(m_DeviceParams.backBufferHeight) };
    AdjustWindowRect(&rect, windowStyle, FALSE);

    if (MoveWindowOntoAdapter(m_DxgiAdapter, rect))
    {
        glfwSetWindowPos(m_Window, rect.left, rect.top);
    }

    m_hWnd = glfwGetWin32Window(m_Window);

    HRESULT hr = E_FAIL;

    RECT clientRect;
    GetClientRect(m_hWnd, &clientRect);
    UINT width = clientRect.right - clientRect.left;
    UINT height = clientRect.bottom - clientRect.top;

    ZeroMemory(&m_SwapChainDesc, sizeof(m_SwapChainDesc));
    m_SwapChainDesc.Width = width;
    m_SwapChainDesc.Height = height;
    m_SwapChainDesc.SampleDesc.Count = m_DeviceParams.swapChainSampleCount;
    m_SwapChainDesc.SampleDesc.Quality = 0;
    m_SwapChainDesc.BufferUsage = m_DeviceParams.swapChainUsage;
    m_SwapChainDesc.BufferCount = m_DeviceParams.swapChainBufferCount;
    m_SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    m_SwapChainDesc.Flags = 0;

    // Special processing for sRGB swap chain formats.
    // DXGI will not create a swap chain with an sRGB format, but its contents will be interpreted as sRGB.
    // So we need to use a non-sRGB format here, but store the true sRGB format for later framebuffer creation.
    switch (m_DeviceParams.swapChainFormat)  // NOLINT(clang-diagnostic-switch-enum)
    {
    case nvrhi::Format::SRGBA8_UNORM:
        m_SwapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case nvrhi::Format::SBGRA8_UNORM:
        m_SwapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    default:
        m_SwapChainDesc.Format = nvrhi::d3d12::convertFormat(m_DeviceParams.swapChainFormat);
        break;
    }

    RefCountPtr<IDXGIFactory5> pDxgiFactory5;
    if (SUCCEEDED(m_DxgiFactory2->QueryInterface(IID_PPV_ARGS(&pDxgiFactory5))))
    {
        BOOL supported = 0;
        if (SUCCEEDED(pDxgiFactory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported))))
            m_TearingSupported = (supported != 0);
    }

    if (m_TearingSupported)
    {
        m_SwapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    m_FullScreenDesc = {};
    m_FullScreenDesc.RefreshRate.Numerator = m_DeviceParams.refreshRate;
    m_FullScreenDesc.RefreshRate.Denominator = 1;
    m_FullScreenDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
    m_FullScreenDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    m_FullScreenDesc.Windowed = TRUE; // DXGI always windowed; fullscreen handled by GLFW
    
    RefCountPtr<IDXGISwapChain1> pSwapChain1;
    hr = m_DxgiFactory2->CreateSwapChainForHwnd(m_GraphicsQueue, m_hWnd, &m_SwapChainDesc, &m_FullScreenDesc, nullptr, &pSwapChain1);
    HR_RETURN(hr)

	hr = pSwapChain1->QueryInterface(IID_PPV_ARGS(&m_SwapChain));
	HR_RETURN(hr)

    m_DxgiFactory2->MakeWindowAssociation(m_hWnd, DXGI_MWA_NO_ALT_ENTER);

    if (!CreateRenderTargets())
        return false;

    hr = m_Device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_FrameFence));
    HR_RETURN(hr)

    for(UINT bufferIndex = 0; bufferIndex < m_SwapChainDesc.BufferCount; bufferIndex++)
    {
        m_FrameFenceEvents.push_back( CreateEvent(nullptr, false, true, nullptr) );
    }

    return true;
}

void DeviceManager_DX12::DestroyDeviceAndSwapChain()
{
    m_RhiSwapChainBuffers.clear();
    m_RendererString.clear();

    ReleaseRenderTargets();

    m_NvrhiDevice = nullptr;

    for (auto fenceEvent : m_FrameFenceEvents)
    {
        WaitForSingleObject(fenceEvent, INFINITE);
        CloseHandle(fenceEvent);
    }

    m_FrameFenceEvents.clear();

    if (m_SwapChain)
    {
        m_SwapChain->SetFullscreenState(false, nullptr);
    }

    m_SwapChainBuffers.clear();

    m_FrameFence = nullptr;
    m_SwapChain = nullptr;
    m_GraphicsQueue = nullptr;
    m_ComputeQueue = nullptr;
    m_CopyQueue = nullptr;
    m_Device12 = nullptr;
}

bool DeviceManager_DX12::CreateRenderTargets()
{
    m_SwapChainBuffers.resize(m_SwapChainDesc.BufferCount);
    m_RhiSwapChainBuffers.resize(m_SwapChainDesc.BufferCount);

    for(UINT n = 0; n < m_SwapChainDesc.BufferCount; n++)
    {
        const HRESULT hr = m_SwapChain->GetBuffer(n, IID_PPV_ARGS(&m_SwapChainBuffers[n]));
        HR_RETURN(hr)

        nvrhi::TextureDesc textureDesc;
        textureDesc.width = m_DeviceParams.backBufferWidth;
        textureDesc.height = m_DeviceParams.backBufferHeight;
        textureDesc.sampleCount = m_DeviceParams.swapChainSampleCount;
        textureDesc.sampleQuality = m_DeviceParams.swapChainSampleQuality;
        textureDesc.format = m_DeviceParams.swapChainFormat;
        textureDesc.debugName = "SwapChainBuffer";
        textureDesc.isRenderTarget = true;
        textureDesc.isUAV = false;
        textureDesc.initialState = nvrhi::ResourceStates::Present;
        textureDesc.keepInitialState = true;

        m_RhiSwapChainBuffers[n] = m_NvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(m_SwapChainBuffers[n]), textureDesc);
    }

    return true;
}

void DeviceManager_DX12::ReleaseRenderTargets()
{
    if (m_NvrhiDevice)
    {
        // Make sure that all frames have finished rendering
        m_NvrhiDevice->waitForIdle();

        // Release all in-flight references to the render targets
        m_NvrhiDevice->runGarbageCollection();
    }

    // Set the events so that WaitForSingleObject in OneFrame will not hang later
    for(auto e : m_FrameFenceEvents)
        SetEvent(e);

    // Release the old buffers because ResizeBuffers requires that
    m_RhiSwapChainBuffers.clear();
    m_SwapChainBuffers.clear();
}

void DeviceManager_DX12::ResizeSwapChain()
{
    ReleaseRenderTargets();

    if (!m_NvrhiDevice)
        return;

    if (!m_SwapChain)
        return;

    const HRESULT hr = m_SwapChain->ResizeBuffers(m_DeviceParams.swapChainBufferCount,
                                            m_DeviceParams.backBufferWidth,
                                            m_DeviceParams.backBufferHeight,
                                            m_SwapChainDesc.Format,
                                            m_SwapChainDesc.Flags);

    if (FAILED(hr))
    {
        donut::log::fatal("ResizeBuffers failed");
    }

    bool ret = CreateRenderTargets();
    if (!ret)
    {
        donut::log::fatal("CreateRenderTarget failed");
    }
}

bool DeviceManager_DX12::BeginFrame()
{
    auto bufferIndex = m_SwapChain->GetCurrentBackBufferIndex();

    WaitForSingleObject(m_FrameFenceEvents[bufferIndex], INFINITE);

    return true;
}

nvrhi::ITexture* DeviceManager_DX12::GetCurrentBackBuffer()
{
    return m_RhiSwapChainBuffers[m_SwapChain->GetCurrentBackBufferIndex()];
}

nvrhi::ITexture* DeviceManager_DX12::GetBackBuffer(uint32_t index)
{
    if (index < m_RhiSwapChainBuffers.size())
        return m_RhiSwapChainBuffers[index];
    return nullptr;
}

uint32_t DeviceManager_DX12::GetCurrentBackBufferIndex()
{
    return m_SwapChain->GetCurrentBackBufferIndex();
}

uint32_t DeviceManager_DX12::GetBackBufferCount()
{
    return m_SwapChainDesc.BufferCount;
}

bool DeviceManager_DX12::Present()
{
    if (!m_windowVisible)
        return true;

    auto bufferIndex = m_SwapChain->GetCurrentBackBufferIndex();

    UINT presentFlags = 0;
    if (!m_DeviceParams.vsyncEnabled && m_FullScreenDesc.Windowed && m_TearingSupported)
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;

    HRESULT result = m_SwapChain->Present(m_DeviceParams.vsyncEnabled ? 1 : 0, presentFlags);

    m_FrameFence->SetEventOnCompletion(m_FrameCount, m_FrameFenceEvents[bufferIndex]);
    m_GraphicsQueue->Signal(m_FrameFence, m_FrameCount);
    m_FrameCount++;
    return SUCCEEDED(result);
}

void DeviceManager_DX12::Shutdown()
{
    DeviceManager::Shutdown();

    m_DxgiAdapter = nullptr;
    m_DxgiFactory2 = nullptr;

    if (m_DeviceParams.enableDebugRuntime)
    {
        ReportLiveObjects();
    }
}

DeviceManager *DeviceManager::CreateD3D12(void)
{
    return new DeviceManager_DX12();
}
