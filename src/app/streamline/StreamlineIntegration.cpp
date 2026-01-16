/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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
#if DONUT_WITH_STREAMLINE
#include <StreamlineIntegration.h>

#include <sl_consts.h>
#include <sl_hooks.h>
#include <sl_version.h>

#define STREAMLINE_HAS_DLSS_RR (SL_VERSION_MAJOR >= 2 && (SL_VERSION_MAJOR > 2 || SL_VERSION_MINOR >= 7))

// Streamline Features
#include <sl_dlss.h>
#include <sl_reflex.h>
#include <sl_nis.h>
#include <sl_dlss_g.h>
#include <sl_deepdvc.h>
#if STREAMLINE_HAS_DLSS_RR
#include <sl_dlss_d.h>
#endif

#include <donut/core/log.h>
#include <filesystem>
#include <map>
#include <dxgi.h>
#include <dxgi1_5.h>

#if DONUT_WITH_DX11
#include <d3d11.h>
#include <nvrhi/d3d11.h>
#endif
#if DONUT_WITH_DX12
#include <d3d12.h>
#include <nvrhi/d3d12.h>
#endif
#if DONUT_WITH_VULKAN
#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#include <vulkan/vulkan.hpp>
#include <nvrhi/vulkan.h>
#include <sl_helpers_vk.h>
#endif

#include "sl_security.h"

#ifndef _WIN32
#include <unistd.h>
#include <cstdio>
#include <climits>
#else
#define PATH_MAX MAX_PATH
#endif // _WIN32


// Set this to a game's specific sdk version
static constexpr uint64_t SDK_VERSION = sl::kSDKVersion;

using namespace donut;
using namespace donut::engine;

namespace donut::app 
{

// Format conversion functions
static sl::float2 make_sl_float2(const dm::float2& donutF) 
{ 
    return sl::float2{ donutF.x, donutF.y }; 
}

static sl::float3 make_sl_float3(const dm::float3& donutF)
{ 
    return sl::float3{ donutF.x, donutF.y, donutF.z }; 
}

static sl::float4 make_sl_float4(const dm::float4& donutF)
{ 
    return sl::float4{ donutF.x, donutF.y, donutF.z, donutF.w }; 
}

static sl::float4x4 make_sl_float4x4(const dm::float4x4& donutF4x4)
{
    sl::float4x4 outF4x4;
    outF4x4.setRow(0, make_sl_float4(donutF4x4.row0));
    outF4x4.setRow(1, make_sl_float4(donutF4x4.row1));
    outF4x4.setRow(2, make_sl_float4(donutF4x4.row2));
    outF4x4.setRow(3, make_sl_float4(donutF4x4.row3));
    return outF4x4;
}

static sl::Boolean make_sl_bool(bool value) 
{ 
    return value ? sl::Boolean::eTrue : sl::Boolean::eFalse; 
}

static void logFunctionCallback(sl::LogType type, const char* msg)
{
    if (type == sl::LogType::eError)
    {
        // Add a breakpoint here to break on errors
        donut::log::error(msg);
    }
    if (type == sl::LogType::eWarn)
    {
        // Add a breakpoint here to break on warnings
        donut::log::warning(msg);
    }
    else
    {
        donut::log::info(msg);
    }
}

static const std::map< const sl::Result, const std::string> errors = 
{
        {sl::Result::eErrorIO,"eErrorIO"},
        {sl::Result::eErrorDriverOutOfDate,"eErrorDriverOutOfDate"},
        {sl::Result::eErrorOSOutOfDate,"eErrorOSOutOfDate"},
        {sl::Result::eErrorOSDisabledHWS,"eErrorOSDisabledHWS"},
        {sl::Result::eErrorDeviceNotCreated,"eErrorDeviceNotCreated"},
        {sl::Result::eErrorAdapterNotSupported,"eErrorAdapterNotSupported"},
        {sl::Result::eErrorNoPlugins,"eErrorNoPlugins"},
        {sl::Result::eErrorVulkanAPI,"eErrorVulkanAPI"},
        {sl::Result::eErrorDXGIAPI,"eErrorDXGIAPI"},
        {sl::Result::eErrorD3DAPI,"eErrorD3DAPI"},
        {sl::Result::eErrorNRDAPI,"eErrorNRDAPI"},
        {sl::Result::eErrorNVAPI,"eErrorNVAPI"},
        {sl::Result::eErrorReflexAPI,"eErrorReflexAPI"},
        {sl::Result::eErrorNGXFailed,"eErrorNGXFailed"},
        {sl::Result::eErrorJSONParsing,"eErrorJSONParsing"},
        {sl::Result::eErrorMissingProxy,"eErrorMissingProxy"},
        {sl::Result::eErrorMissingResourceState,"eErrorMissingResourceState"},
        {sl::Result::eErrorInvalidIntegration,"eErrorInvalidIntegration"},
        {sl::Result::eErrorMissingInputParameter,"eErrorMissingInputParameter"},
        {sl::Result::eErrorNotInitialized,"eErrorNotInitialized"},
        {sl::Result::eErrorComputeFailed,"eErrorComputeFailed"},
        {sl::Result::eErrorInitNotCalled,"eErrorInitNotCalled"},
        {sl::Result::eErrorExceptionHandler,"eErrorExceptionHandler"},
        {sl::Result::eErrorInvalidParameter,"eErrorInvalidParameter"},
        {sl::Result::eErrorMissingConstants,"eErrorMissingConstants"},
        {sl::Result::eErrorDuplicatedConstants,"eErrorDuplicatedConstants"},
        {sl::Result::eErrorMissingOrInvalidAPI,"eErrorMissingOrInvalidAPI"},
        {sl::Result::eErrorCommonConstantsMissing,"eErrorCommonConstantsMissing"},
        {sl::Result::eErrorUnsupportedInterface,"eErrorUnsupportedInterface"},
        {sl::Result::eErrorFeatureMissing,"eErrorFeatureMissing"},
        {sl::Result::eErrorFeatureNotSupported,"eErrorFeatureNotSupported"},
        {sl::Result::eErrorFeatureMissingHooks,"eErrorFeatureMissingHooks"},
        {sl::Result::eErrorFeatureFailedToLoad,"eErrorFeatureFailedToLoad"},
        {sl::Result::eErrorFeatureWrongPriority,"eErrorFeatureWrongPriority"},
        {sl::Result::eErrorFeatureMissingDependency,"eErrorFeatureMissingDependency"},
        {sl::Result::eErrorFeatureManagerInvalidState,"eErrorFeatureManagerInvalidState"},
        {sl::Result::eErrorInvalidState,"eErrorInvalidState"}
};

static const std::map< const sl::Result, const std::string> warnings =
{
        {sl::Result::eWarnOutOfVRAM,"eWarnOutOfVRAM"}
};

static bool successCheck(sl::Result result, const char* location)
{
    if (result == sl::Result::eOk)
        return true;

    auto a = errors.find(result);
    auto w = warnings.find(result);
    if (w != warnings.end())
    {
        logFunctionCallback(sl::LogType::eWarn, ("Warning: " + w->second + (location == nullptr ? "" : (" encountered in " + std::string(location)))).c_str());
        return true;
    } 
    else if (a != errors.end())
        logFunctionCallback(sl::LogType::eError, ("Error: " + a->second + (location == nullptr ? "" : (" encountered in " + std::string(location)))).c_str());
    else
        logFunctionCallback(sl::LogType::eError, ("Unknown error " + static_cast<int>(result) + (location == nullptr ? "" : (" encountered in " + std::string(location)))).c_str());

    return false;
}

static std::wstring GetSlInterposerDllLocation()
{
    wchar_t path[PATH_MAX] = { 0 };
#ifdef _WIN32
    if (GetModuleFileNameW(nullptr, path, dim(path)) == 0)
        return std::wstring();
#else // _WIN32
#error Unsupported platform for GetSlInterposerDllLocation!
#endif // _WIN32

    auto basePath = std::filesystem::path(path).parent_path();
    auto dllPath = basePath.wstring().append(L"\\sl.interposer.dll");
    return dllPath;
}

StreamlineIntegration& StreamlineIntegration::Get()
{
    static StreamlineIntegration instance;
    return instance;
}

bool StreamlineIntegration::InitializePreDevice(nvrhi::GraphicsAPI api, int appId, const bool checkSig, const bool enableLog)
{
    if (m_slInitialized)
    {
        log::info("StreamlineIntegration is already initialised.");
        return true;
    }

    sl::Preferences pref;

    m_api = api;

    if (m_api != nvrhi::GraphicsAPI::VULKAN)
    {
        pref.allocateCallback = &AllocateResourceCallback;
        pref.releaseCallback = &ReleaseResourceCallback;
    }

    pref.applicationId = appId;

    if (enableLog)
    {
        pref.showConsole = true;
        pref.logMessageCallback = &logFunctionCallback;
        pref.logLevel = sl::LogLevel::eDefault;
    }
    else
    {
        pref.logLevel = sl::LogLevel::eOff;
    }

    sl::Feature featuresToLoad[] = {
#if STREAMLINE_FEATURE_DLSS_SR
        sl::kFeatureDLSS,
#endif
#if STREAMLINE_FEATURE_NIS
        sl::kFeatureNIS,
#endif
#if STREAMLINE_FEATURE_DLSS_FG
        sl::kFeatureDLSS_G,
#endif
#if STREAMLINE_FEATURE_REFLEX
        sl::kFeatureReflex,
#endif
#if STREAMLINE_FEATURE_DEEPDVC
        sl::kFeatureDeepDVC,
#endif
#if STREAMLINE_FEATURE_DLSS_RR && STREAMLINE_HAS_DLSS_RR
        sl::kFeatureDLSS_RR,
#endif
        // PCL is always implicitly loaded, but request it to ensure we never have 0-sized array
        sl::kFeaturePCL
    };
    pref.featuresToLoad = featuresToLoad;
    pref.numFeaturesToLoad = uint32_t(std::size(featuresToLoad));

    switch (api)
    {
    case (nvrhi::GraphicsAPI::D3D11):
        pref.renderAPI = sl::RenderAPI::eD3D11;
        break;
    case (nvrhi::GraphicsAPI::D3D12):
        pref.renderAPI = sl::RenderAPI::eD3D12;
        break;
    case (nvrhi::GraphicsAPI::VULKAN):
        pref.renderAPI = sl::RenderAPI::eVulkan;
        break;
    }

    pref.flags |= sl::PreferenceFlags::eUseManualHooking;

    auto pathDll = GetSlInterposerDllLocation();

    HMODULE interposer = {};
    if (!checkSig || sl::security::verifyEmbeddedSignature(pathDll.c_str()))
    {
        interposer = LoadLibraryW(pathDll.c_str());
    }

    if (!interposer)
    {
        donut::log::error("Unable to load Streamline Interposer");
        return false;
    }

    m_slInitialized = successCheck(slInit(pref, SDK_VERSION), "slInit");
    if (!m_slInitialized)
    {
        log::error("Failed to initialise SL.");
        return false;
    }

    return true;
}

#if DONUT_WITH_DX11 || DONUT_WITH_DX12
bool StreamlineIntegration::SetD3DDevice(IUnknown* nativeDevice)
{
    bool result = successCheck(slSetD3DDevice(nativeDevice), "slSetD3DDevice");
    if (!result)
    {
        log::error("Failed to initialise SL.");
    }
    return result;
}
#endif

#if DONUT_WITH_DX11 || DONUT_WITH_DX12
bool StreamlineIntegration::InitializeDeviceDX(nvrhi::IDevice *device, AdapterInfo::LUID *pAdapterIdDx11)
{
    m_device = device;

#if DONUT_WITH_DX11
    if (m_api == nvrhi::GraphicsAPI::D3D11 && pAdapterIdDx11)
    {
        assert(pAdapterIdDx11->size() == sizeof(m_d3d11Luid));
        memcpy(&m_d3d11Luid, pAdapterIdDx11->data(), pAdapterIdDx11->size());
    }
#endif

    UpdateFeatureAvailable();
    return true;
}
#endif

#if DONUT_WITH_DX11 || DONUT_WITH_DX12
bool StreamlineIntegration::UpgradeInterface(IUnknown*& interfacePointer)
{
    IUnknown* nativeInterface = interfacePointer;
    sl::Result slRes = slUpgradeInterface((void**)&interfacePointer);
    if (slRes != sl::Result::eOk)
    {
        donut::log::error("slUpgradeInterface failed.\n");
        return false;
    }
    // Release the native interface
    nativeInterface->Release();
    return true;
}
#endif

#if DONUT_WITH_VULKAN
bool StreamlineIntegration::InitializeDeviceVK(nvrhi::IDevice* device, const VulkanInfo& vulkanInfo)
{
    m_device = device;

    UpdateFeatureAvailable();
    return true;
}
#endif

uint32_t StreamlineIntegration::CheckNumSupportedFeatures(const sl::AdapterInfo &adapterInfo)
{
    auto checkFeature = [this, &adapterInfo](sl::Feature feature, std::string feature_name) -> bool
        {
            sl::Result res = slIsFeatureSupported(feature, adapterInfo);
            if (res == sl::Result::eOk)
            {
                log::info((feature_name + " is supported on this adapter").c_str());
            }
            else
            {
                std::string errorType{};
                auto a = errors.find(res);
                if (a != errors.end())
                {
                    errorType = a->second;
                }

                log::info((feature_name + " is NOT supported on this adapter with error: " + errorType).c_str());
            }
            return (res == sl::Result::eOk);
        };


    uint32_t supportedSLFeatureCnt = 0;

#if STREAMLINE_FEATURE_DLSS_SR
    supportedSLFeatureCnt += static_cast<uint32_t>(checkFeature(sl::kFeatureDLSS, "DLSS"));
#endif
#if STREAMLINE_FEATURE_NIS
    supportedSLFeatureCnt += static_cast<uint32_t>(checkFeature(sl::kFeatureNIS, "NIS"));
#endif
#if STREAMLINE_FEATURE_DLSS_FG
    supportedSLFeatureCnt += static_cast<uint32_t>(checkFeature(sl::kFeatureDLSS_G, "DLSS_G"));
#endif
#if STREAMLINE_FEATURE_REFLEX
    supportedSLFeatureCnt += static_cast<uint32_t>(checkFeature(sl::kFeatureReflex, "Reflex"));
#endif
#if STREAMLINE_FEATURE_DEEPDVC
    supportedSLFeatureCnt += static_cast<uint32_t>(checkFeature(sl::kFeatureDeepDVC, "DeepDVC"));
#endif
#if STREAMLINE_FEATURE_DLSS_RR && STREAMLINE_HAS_DLSS_RR
    supportedSLFeatureCnt += static_cast<uint32_t>(checkFeature(sl::kFeatureDLSS_RR, "DLSS_RR"));
#endif

    return supportedSLFeatureCnt;
}

#if DONUT_WITH_DX11 || DONUT_WITH_DX12
uint32_t StreamlineIntegration::FindBestAdapterDX()
{
    uint32_t foundAdapter = 0;
    int maxSLSupportedFeatures = -1;

    if (m_api == nvrhi::GraphicsAPI::D3D11 || m_api == nvrhi::GraphicsAPI::D3D12)
    {
        IDXGIFactory1* DXGIFactory;
        HRESULT hres = CreateDXGIFactory1(IID_PPV_ARGS(&DXGIFactory));
        if (hres != S_OK)
        {
            donut::log::info("failed to CreateDXGIFactory when finding adapters.\n");
            return foundAdapter;
        }

        IDXGIAdapter* pBestAdapter = nullptr;
        DXGI_ADAPTER_DESC bestAdapterDesc = {};
        uint32_t adapterIndex = 0;
        IDXGIAdapter* pAdapter = nullptr;

        while (true)
        {
            hres = DXGIFactory->EnumAdapters(adapterIndex, &pAdapter);

            if (!(hres == S_OK))
                break;

            DXGI_ADAPTER_DESC adapterDesc;
            pAdapter->GetDesc(&adapterDesc);

            sl::AdapterInfo adapterInfo{};
            adapterInfo.deviceLUID = (uint8_t*)&adapterDesc.AdapterLuid;
            adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

            log::info("Found adapter: %S, DeviceId=0x%X, Vendor: %i", adapterDesc.Description, adapterDesc.DeviceId, adapterDesc.VendorId);

            int supportedSLFeatureCnt = CheckNumSupportedFeatures(adapterInfo);
            if (supportedSLFeatureCnt > maxSLSupportedFeatures)
            {
                pBestAdapter = pAdapter;
                foundAdapter = adapterIndex;
                bestAdapterDesc = adapterDesc;
                maxSLSupportedFeatures = supportedSLFeatureCnt;
            }

            adapterIndex++;
        }

        if (pBestAdapter != nullptr)
        {
            log::info("Using adapter: %S, DeviceId=0x%X, Vendor: %i", bestAdapterDesc.Description, bestAdapterDesc.DeviceId, bestAdapterDesc.VendorId);
        }
        else
        {
            log::info("No ideal adapter was found, we will use the default adapter.");
        }

        if (DXGIFactory)
            DXGIFactory->Release();
    }
    return foundAdapter;
}
#endif

#if DONUT_WITH_VULKAN
uint32_t StreamlineIntegration::FindBestAdapterVulkan(const std::vector <vk::PhysicalDevice> &vkDevices)
{
    uint32_t foundAdapter = 0;
    int maxSLSupportedFeatures = -1;

    if (m_api == nvrhi::GraphicsAPI::VULKAN)
    {
        const vk::PhysicalDevice* pBestAdapter = nullptr;
        vk::PhysicalDeviceProperties bestAdapterDesc;

        uint32_t adapterIndex = 0;
        for (auto& devicePtr : vkDevices)
        {
            sl::AdapterInfo adapterInfo{};
            adapterInfo.vkPhysicalDevice = devicePtr;

            auto adapterDesc = ((vk::PhysicalDevice)devicePtr).getProperties();
            auto str = adapterDesc.deviceName.data();
            log::info("Found adapter: %s, DeviceId=0x%X, Vendor: %i", str, adapterDesc.deviceID, adapterDesc.vendorID);

            int supportedSLFeatureCnt = CheckNumSupportedFeatures(adapterInfo);
            if (supportedSLFeatureCnt > maxSLSupportedFeatures)
            {
                pBestAdapter = &devicePtr;
                bestAdapterDesc = adapterDesc;
                maxSLSupportedFeatures = supportedSLFeatureCnt;
                foundAdapter = adapterIndex;
            }
            adapterIndex++;
        }

        if (pBestAdapter != nullptr)
        {
            auto str = bestAdapterDesc.deviceName.data();
            log::info("Using adapter: %s, DeviceId=0x%X, Vendor: %i", str, bestAdapterDesc.deviceID, bestAdapterDesc.vendorID);
        }
        else
        {
            log::info("No ideal adapter was found, we will use the default adapter.");
        }
    }
    return foundAdapter;
}
#endif

void StreamlineIntegration::UpdateFeatureAvailable()
{
    sl::AdapterInfo adapterInfo;

#if DONUT_WITH_DX11
    if (m_api == nvrhi::GraphicsAPI::D3D11)
    {
        adapterInfo.deviceLUID = (uint8_t*)&m_d3d11Luid;
        adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);
    }
#endif
#if DONUT_WITH_DX12
    if (m_api == nvrhi::GraphicsAPI::D3D12)
    {
        auto a = ((ID3D12Device*)m_device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device))->GetAdapterLuid();
        adapterInfo.deviceLUID = (uint8_t*)&a;
        adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);
    }
#endif
#if DONUT_WITH_VULKAN
    if (m_api == nvrhi::GraphicsAPI::VULKAN)
    {
        adapterInfo.vkPhysicalDevice = m_device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice);
    }
#endif

    // Check if features are fully functional (2nd call of slIsFeatureSupported onwards)
#if STREAMLINE_FEATURE_DLSS_SR
    m_dlssAvailable = sl::Result::eOk == slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo), "slIsFeatureSupported_DLSS";
    if (m_dlssAvailable) log::info("DLSS is supported on this system.");
    else log::warning("DLSS is not fully functional on this system.");
#endif

#if STREAMLINE_FEATURE_NIS
    m_nisAvailable = sl::Result::eOk == slIsFeatureSupported(sl::kFeatureNIS, adapterInfo), "slIsFeatureSupported_NIS";
    if (m_nisAvailable) log::info("NIS is supported on this system.");
    else log::warning("NIS is not fully functional on this system.");
#endif

#if STREAMLINE_FEATURE_DLSS_FG
    m_dlssgAvailable = sl::Result::eOk == slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo), "slIsFeatureSupported_DLSSG";
    if (m_dlssgAvailable) log::info("DLSS-G is supported on this system.");
    else log::warning("DLSS-G is not fully functional on this system.");
#endif

    m_pclAvailable = sl::Result::eOk == slIsFeatureSupported(sl::kFeaturePCL, adapterInfo), "slIsFeatureSupported_PCL";
    if (m_pclAvailable) log::info("PCL is supported on this system.");
    else log::warning("PCL is not fully functional on this system.");

#if STREAMLINE_FEATURE_REFLEX
    m_reflexAvailable = sl::Result::eOk == slIsFeatureSupported(sl::kFeatureReflex, adapterInfo), "slIsFeatureSupported_REFLEX";
    if (m_reflexAvailable) log::info("Reflex is supported on this system.");
    else log::warning("Reflex is not fully functional on this system.");
#endif

#if STREAMLINE_FEATURE_DEEPDVC
    m_deepdvcAvailable = sl::Result::eOk == slIsFeatureSupported(sl::kFeatureDeepDVC, adapterInfo), "slIsFeatureSupported_DeepDVC";
    if (m_deepdvcAvailable) log::info("DeepDVC is supported on this system.");
    else log::warning("DeepDVC is not fully functional on this system.");
#endif

#if STREAMLINE_FEATURE_DLSS_RR && STREAMLINE_HAS_DLSS_RR
    m_dlssrrAvailable = sl::Result::eOk == slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo), "slIsFeatureSupported_DLSSRR";
    if (m_dlssrrAvailable) log::info("DLSS-RR is supported on this system.");
    else log::warning("DLSS-RR is not fully functional on this system.");
#endif
}


void StreamlineIntegration::Shutdown()
{
    // Un-set all tags
    sl::ResourceTag inputs[] = 
    {
        sl::ResourceTag{nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeNormals, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeRoughness, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeSpecularMotionVectors, sl::ResourceLifecycle::eValidUntilPresent}
    };
    successCheck(slSetTag(m_viewport, inputs, _countof(inputs), nullptr), "slSetTag_clear");

    // Shutdown Streamline
    if (m_slInitialized)
    {
        successCheck(slShutdown(), "slShutdown");
        m_slInitialized = false;
    }
}

void StreamlineIntegration::SetViewport(uint32_t viewportIndex)
{
    m_viewport = sl::ViewportHandle(viewportIndex);
}

void StreamlineIntegration::SetConstants(const Constants& consts)
{
    sl::Constants slConstants;
    slConstants.cameraViewToClip = make_sl_float4x4(consts.cameraViewToClip);
    slConstants.clipToCameraView = make_sl_float4x4(consts.clipToCameraView);
    slConstants.clipToLensClip = make_sl_float4x4(consts.clipToLensClip);
    slConstants.clipToPrevClip = make_sl_float4x4(consts.clipToPrevClip);
    slConstants.prevClipToClip = make_sl_float4x4(consts.prevClipToClip);
    slConstants.jitterOffset = make_sl_float2(consts.jitterOffset);
    slConstants.mvecScale = make_sl_float2(consts.mvecScale);
    slConstants.cameraPinholeOffset = make_sl_float2(consts.cameraPinholeOffset);
    slConstants.cameraPos = make_sl_float3(consts.cameraPos);
    slConstants.cameraUp = make_sl_float3(consts.cameraUp);
    slConstants.cameraRight = make_sl_float3(consts.cameraRight);
    slConstants.cameraFwd = make_sl_float3(consts.cameraFwd);
    slConstants.cameraNear = consts.cameraNear;
    slConstants.cameraFar = consts.cameraFar;
    slConstants.cameraFOV = consts.cameraFOV;
    slConstants.cameraAspectRatio = consts.cameraAspectRatio;
    slConstants.motionVectorsInvalidValue = consts.motionVectorsInvalidValue;
    slConstants.depthInverted = make_sl_bool(consts.depthInverted);
    slConstants.cameraMotionIncluded = make_sl_bool(consts.cameraMotionIncluded);
    slConstants.motionVectors3D = make_sl_bool(consts.motionVectors3D);
    slConstants.reset = make_sl_bool(consts.reset);
    slConstants.orthographicProjection = make_sl_bool(consts.orthographicProjection);
    slConstants.motionVectorsDilated = make_sl_bool(consts.motionVectorsDilated);
    slConstants.motionVectorsJittered = make_sl_bool(consts.motionVectorsJittered);
    slConstants.minRelativeLinearDepthObjectSeparation = consts.minRelativeLinearDepthObjectSeparation;

    if (!m_slInitialized)
    {
        log::warning("SL not initialised.");
        return;
    }

    successCheck(slSetConstants(slConstants, *m_currentFrame, m_viewport), "slSetConstants");
}

static sl::DLSSOptions ConvertOptions(const StreamlineIntegration::DLSSOptions& options)
{
    static_assert(sl::DLSSPreset::eDefault == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::eDefault);
#if !(SL_VERSION_MAJOR >= 2 && (SL_VERSION_MAJOR > 2 || SL_VERSION_MINOR >= 9))
    static_assert(sl::DLSSPreset::ePresetA == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetA);
    static_assert(sl::DLSSPreset::ePresetB == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetB);
    static_assert(sl::DLSSPreset::ePresetC == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetC);
    static_assert(sl::DLSSPreset::ePresetD == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetD);
    static_assert(sl::DLSSPreset::ePresetE == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetE);
#endif
    static_assert(sl::DLSSPreset::ePresetF == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetF);
    static_assert(sl::DLSSPreset::ePresetG == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetG);

#if (SL_VERSION_MAJOR >= 2 && (SL_VERSION_MAJOR > 2 || SL_VERSION_MINOR >= 7))
    static_assert(sl::DLSSPreset::ePresetH == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetH);
    static_assert(sl::DLSSPreset::ePresetI == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetI);
    static_assert(sl::DLSSPreset::ePresetJ == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetJ);
#endif

#if (SL_VERSION_MAJOR >= 2 && (SL_VERSION_MAJOR > 2 || SL_VERSION_MINOR >= 9))
    static_assert(sl::DLSSPreset::ePresetK == (sl::DLSSPreset)StreamlineIntegration::DLSSPreset::ePresetK);
#endif
    
    static_assert(sl::DLSSMode::eOff == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eOff);
    static_assert(sl::DLSSMode::eMaxPerformance == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eMaxPerformance);
    static_assert(sl::DLSSMode::eBalanced == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eBalanced);
    static_assert(sl::DLSSMode::eMaxQuality == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eMaxQuality);
    static_assert(sl::DLSSMode::eUltraPerformance == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eUltraPerformance);
    static_assert(sl::DLSSMode::eUltraQuality == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eUltraQuality);
    static_assert(sl::DLSSMode::eDLAA == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eDLAA);
    static_assert(sl::DLSSMode::eCount == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eCount);

    sl::DLSSOptions slOptions;
    slOptions.mode = (sl::DLSSMode)options.mode;
    slOptions.outputWidth = options.outputWidth;
    slOptions.outputHeight = options.outputHeight;
    slOptions.sharpness = options.sharpness;
    slOptions.preExposure = options.preExposure;
    slOptions.exposureScale = options.exposureScale;
    slOptions.colorBuffersHDR = make_sl_bool(options.colorBuffersHDR);
    slOptions.indicatorInvertAxisX = make_sl_bool(options.indicatorInvertAxisX);
    slOptions.indicatorInvertAxisY = make_sl_bool(options.indicatorInvertAxisY);

    slOptions.dlaaPreset = sl::DLSSPreset(options.preset);
    slOptions.qualityPreset = sl::DLSSPreset(options.preset);
    slOptions.balancedPreset = sl::DLSSPreset(options.preset);
    slOptions.performancePreset = sl::DLSSPreset(options.preset);
    slOptions.ultraPerformancePreset = sl::DLSSPreset(options.preset);
    slOptions.ultraQualityPreset = sl::DLSSPreset(options.preset);

    slOptions.useAutoExposure = make_sl_bool(options.useAutoExposure);
    slOptions.alphaUpscalingEnabled = make_sl_bool(options.alphaUpscalingEnabled);

    return slOptions;
}

void StreamlineIntegration::SetDLSSOptions(const DLSSOptions& options)
{
    if (!m_slInitialized || !m_dlssAvailable)
    {
        log::warning("SL not initialised or DLSS not available.");
        return;
    }

    successCheck(slDLSSSetOptions(m_viewport, ConvertOptions(options)), "slDLSSSetOptions");
}

void StreamlineIntegration::QueryDLSSOptimalSettings(const DLSSOptions& options, DLSSSettings& settings)
{
    if (!m_slInitialized || !m_dlssAvailable)
    {
        log::warning("SL not initialised or DLSS not available.");
        settings = DLSSSettings{};
        return;
    }

    sl::DLSSOptimalSettings dlssOptimal = {};
    successCheck(slDLSSGetOptimalSettings(ConvertOptions(options), dlssOptimal), "slDLSSGetOptimalSettings");

    settings.optimalRenderSize.x = static_cast<int>(dlssOptimal.optimalRenderWidth);
    settings.optimalRenderSize.y = static_cast<int>(dlssOptimal.optimalRenderHeight);
    settings.sharpness = dlssOptimal.optimalSharpness;

    settings.minRenderSize.x = dlssOptimal.renderWidthMin;
    settings.minRenderSize.y = dlssOptimal.renderHeightMin;
    settings.maxRenderSize.x = dlssOptimal.renderWidthMax;
    settings.maxRenderSize.y = dlssOptimal.renderHeightMax;
}

#if STREAMLINE_HAS_DLSS_RR
static sl::DLSSDOptions ConvertOptions(const StreamlineIntegration::DLSSRROptions& options)
{
    static_assert(sl::DLSSDPreset::eDefault == (sl::DLSSDPreset)StreamlineIntegration::DLSSRRPreset::eDefault);
#if !(SL_VERSION_MAJOR >= 2 && (SL_VERSION_MAJOR > 2 || SL_VERSION_MINOR >= 9))
    static_assert(sl::DLSSDPreset::ePresetA == (sl::DLSSDPreset)StreamlineIntegration::DLSSRRPreset::ePresetA);
    static_assert(sl::DLSSDPreset::ePresetB == (sl::DLSSDPreset)StreamlineIntegration::DLSSRRPreset::ePresetB);
    static_assert(sl::DLSSDPreset::ePresetC == (sl::DLSSDPreset)StreamlineIntegration::DLSSRRPreset::ePresetC);
#endif
    static_assert(sl::DLSSDPreset::ePresetD == (sl::DLSSDPreset)StreamlineIntegration::DLSSRRPreset::ePresetD);
    static_assert(sl::DLSSDPreset::ePresetE == (sl::DLSSDPreset)StreamlineIntegration::DLSSRRPreset::ePresetE);
#if (SL_VERSION_MAJOR >= 2 && (SL_VERSION_MAJOR > 2 || SL_VERSION_MINOR >= 9))
    static_assert(sl::DLSSDPreset::ePresetH == (sl::DLSSDPreset)StreamlineIntegration::DLSSRRPreset::ePresetH);
#endif
    
    static_assert(sl::DLSSMode::eOff == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eOff);
    static_assert(sl::DLSSMode::eMaxPerformance == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eMaxPerformance);
    static_assert(sl::DLSSMode::eBalanced == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eBalanced);
    static_assert(sl::DLSSMode::eMaxQuality == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eMaxQuality);
    static_assert(sl::DLSSMode::eUltraPerformance == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eUltraPerformance);
    static_assert(sl::DLSSMode::eUltraQuality == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eUltraQuality);
    static_assert(sl::DLSSMode::eDLAA == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eDLAA);
    static_assert(sl::DLSSMode::eCount == (sl::DLSSMode)StreamlineIntegration::DLSSMode::eCount);

    static_assert(sl::DLSSDNormalRoughnessMode::eUnpacked == (sl::DLSSDNormalRoughnessMode)StreamlineIntegration::DLSSRRNormalRoughnessMode::eUnpacked);
    static_assert(sl::DLSSDNormalRoughnessMode::ePacked == (sl::DLSSDNormalRoughnessMode)StreamlineIntegration::DLSSRRNormalRoughnessMode::ePacked);

    sl::DLSSDOptions slOptions;
    slOptions.mode = (sl::DLSSMode)options.mode;
    slOptions.outputWidth = options.outputWidth;
    slOptions.outputHeight = options.outputHeight;
    slOptions.sharpness = options.sharpness;
    slOptions.preExposure = options.preExposure;
    slOptions.exposureScale = options.exposureScale;
    slOptions.colorBuffersHDR = make_sl_bool(options.colorBuffersHDR);
    slOptions.indicatorInvertAxisX = make_sl_bool(options.indicatorInvertAxisX);
    slOptions.indicatorInvertAxisY = make_sl_bool(options.indicatorInvertAxisY);
    slOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode(options.normalRoughnessMode);
    
    slOptions.worldToCameraView = make_sl_float4x4(options.worldToCameraView);
    slOptions.cameraViewToWorld = make_sl_float4x4(options.cameraViewToWorld);
    slOptions.alphaUpscalingEnabled = make_sl_bool(options.alphaUpscalingEnabled);

    slOptions.dlaaPreset = sl::DLSSDPreset(options.preset);
    slOptions.qualityPreset = sl::DLSSDPreset(options.preset);
    slOptions.balancedPreset = sl::DLSSDPreset(options.preset);
    slOptions.performancePreset = sl::DLSSDPreset(options.preset);
    slOptions.ultraPerformancePreset = sl::DLSSDPreset(options.preset);
    slOptions.ultraQualityPreset = sl::DLSSDPreset(options.preset);

    return slOptions;
}
#endif

void StreamlineIntegration::SetDLSSRROptions(const DLSSRROptions& options)
{
#if STREAMLINE_HAS_DLSS_RR
    if (!m_slInitialized || !m_dlssrrAvailable)
    {
        log::warning("SL not initialised or DLSS-RR not available.");
        return;
    }

    successCheck(slDLSSDSetOptions(m_viewport, ConvertOptions(options)), "slDLSSSetOptions");
#endif
}

void StreamlineIntegration::QueryDLSSRROptimalSettings(const DLSSRROptions& options, DLSSRRSettings& settings)
{
#if STREAMLINE_HAS_DLSS_RR
    if (!m_slInitialized || !m_dlssrrAvailable)
    {
        log::warning("SL not initialised or DLSS RR is not available.");
        settings = DLSSRRSettings{};
        return;
    }

    sl::DLSSDOptimalSettings dlssOptimal = {};
    successCheck(slDLSSDGetOptimalSettings(ConvertOptions(options), dlssOptimal), "slDLSSGetOptimalSettings");

    settings.optimalRenderSize.x = static_cast<int>(dlssOptimal.optimalRenderWidth);
    settings.optimalRenderSize.y = static_cast<int>(dlssOptimal.optimalRenderHeight);
    settings.sharpness = dlssOptimal.optimalSharpness;

    settings.minRenderSize.x = dlssOptimal.renderWidthMin;
    settings.minRenderSize.y = dlssOptimal.renderHeightMin;
    settings.maxRenderSize.x = dlssOptimal.renderWidthMax;
    settings.maxRenderSize.y = dlssOptimal.renderHeightMax;
#endif
}

void StreamlineIntegration::CleanupDLSS(bool wfi)
{
    if (!m_slInitialized)
    {
        log::warning("SL not initialised.");
        return;
    }

    if (!m_dlssAvailable)
    {
        return;
    }

    if (!m_dlssAvailable)
    {
        log::warning("DLSS not available.");
        return;
    }

    if (wfi)
    {
        m_device->waitForIdle();
    }

    sl::Result status = slFreeResources(sl::kFeatureDLSS, m_viewport);
    // if we've never ran the feature on this viewport, this call may return 'eErrorInvalidParameter'
    assert(status == sl::Result::eOk || status == sl::Result::eErrorInvalidParameter);
}

void StreamlineIntegration::SetNISOptions(const NISOptions& options)
{
    static_assert(sl::NISMode::eOff == (sl::NISMode)NISMode::eOff);
    static_assert(sl::NISMode::eScaler == (sl::NISMode)NISMode::eScaler);
    static_assert(sl::NISMode::eSharpen == (sl::NISMode)NISMode::eSharpen);
    static_assert(sl::NISMode::eCount == (sl::NISMode)NISMode::eCount);

    static_assert(sl::NISHDR::eNone == (sl::NISHDR)NISHDR::eNone);
    static_assert(sl::NISHDR::eLinear == (sl::NISHDR)NISHDR::eLinear);
    static_assert(sl::NISHDR::ePQ == (sl::NISHDR)NISHDR::ePQ);
    static_assert(sl::NISHDR::eCount == (sl::NISHDR)NISHDR::eCount);

    sl::NISOptions slOptions;
    slOptions.mode = (sl::NISMode)options.mode;
    slOptions.hdrMode = (sl::NISHDR)options.hdrMode;
    slOptions.sharpness = options.sharpness;

    if (!m_slInitialized || !m_nisAvailable)
    {
        log::warning("SL not initialised or DLSS not available.");
        return;
    }

    successCheck(slNISSetOptions(m_viewport, slOptions), "slNISSetOptions");
}

void StreamlineIntegration::CleanupNIS(bool wfi)
{
    if (!m_slInitialized)
    {
        log::warning("SL not initialised.");
        return;
    }

    if (!m_nisAvailable)
    {
        log::warning("NIS not available.");
        return;
    }

    if (wfi)
    {
        m_device->waitForIdle();
    }

    successCheck(slFreeResources(sl::kFeatureNIS, m_viewport), "slFreeResources_NIS");
}

void StreamlineIntegration::SetDeepDVCOptions(const DeepDVCOptions& options)
{
    static_assert(sl::DeepDVCMode::eOff == (sl::DeepDVCMode)DeepDVCMode::eOff);
    static_assert(sl::DeepDVCMode::eOn == (sl::DeepDVCMode)DeepDVCMode::eOn);
    static_assert(sl::DeepDVCMode::eCount == (sl::DeepDVCMode)DeepDVCMode::eCount);

    sl::DeepDVCOptions slOptions;
    slOptions.mode = (sl::DeepDVCMode)options.mode;
    slOptions.intensity = options.intensity;
    slOptions.saturationBoost = options.saturationBoost;

    if (!m_slInitialized || !m_deepdvcAvailable)
    {
        log::warning("SL not initialised or DeepDVC not available.");
        return;
    }

    successCheck(slDeepDVCSetOptions(m_viewport, slOptions), "slDeepDVCSetOptions");
}

void StreamlineIntegration::CleanupDeepDVC()
{
    if (!m_slInitialized)
    {
        log::warning("SL not initialised.");
        return;
    }

    if (!m_deepdvcAvailable)
    {
        log::warning("DeepDVC not available.");
        return;
    }

    m_device->waitForIdle();
    successCheck(slFreeResources(sl::kFeatureDeepDVC, m_viewport), "slFreeResources_DeepDVC");
}

void StreamlineIntegration::SetDLSSGOptions(const DLSSGOptions& options)
{
    if (!m_slInitialized || !m_dlssgAvailable)
    {
        log::warning("SL not initialised or DLSSG not available.");
        return;
    }

    sl::DLSSGOptions slOptions;
    slOptions.mode = (sl::DLSSGMode)options.mode;
    slOptions.numFramesToGenerate = options.numFramesToGenerate;
    slOptions.flags = (sl::DLSSGFlags)options.flags;
    slOptions.dynamicResWidth = options.dynamicResWidth;
    slOptions.dynamicResHeight = options.dynamicResHeight;
    slOptions.numBackBuffers = options.numBackBuffers;
    slOptions.mvecDepthWidth = options.mvecDepthWidth;
    slOptions.mvecDepthHeight = options.mvecDepthHeight;
    slOptions.colorWidth = options.colorWidth;
    slOptions.colorHeight = options.colorHeight;
    slOptions.colorBufferFormat = options.colorBufferFormat;
    slOptions.mvecBufferFormat = options.mvecBufferFormat;
    slOptions.depthBufferFormat = options.depthBufferFormat;
    slOptions.hudLessBufferFormat = options.hudLessBufferFormat;
    slOptions.uiBufferFormat = options.uiBufferFormat;
    slOptions.onErrorCallback = nullptr; // donut does not expose this

#if (SL_VERSION_MAJOR >= 2 && (SL_VERSION_MAJOR > 2 || SL_VERSION_MINOR >= 7))
    slOptions.queueParallelismMode = (sl::DLSSGQueueParallelismMode)options.queueParallelismMode;
#endif

    successCheck(slDLSSGSetOptions(m_viewport, slOptions), "slDLSSGSetOptions");
}

void StreamlineIntegration::GetDLSSGState(DLSSGState& state, const DLSSGOptions& options)
{
    if (!m_slInitialized || !m_dlssgAvailable)
    {
        log::warning("SL not initialised or DLSSG not available.");
        return;
    }

    sl::DLSSGOptions slOptions;
    slOptions.mode = (sl::DLSSGMode)options.mode;
    slOptions.numFramesToGenerate = options.numFramesToGenerate;
    slOptions.flags = (sl::DLSSGFlags)options.flags;
    slOptions.dynamicResWidth = options.dynamicResWidth;
    slOptions.dynamicResHeight = options.dynamicResHeight;
    slOptions.numBackBuffers = options.numBackBuffers;
    slOptions.mvecDepthWidth = options.mvecDepthWidth;
    slOptions.mvecDepthHeight = options.mvecDepthHeight;
    slOptions.colorWidth = options.colorWidth;
    slOptions.colorHeight = options.colorHeight;
    slOptions.colorBufferFormat = options.colorBufferFormat;
    slOptions.mvecBufferFormat = options.mvecBufferFormat;
    slOptions.depthBufferFormat = options.depthBufferFormat;
    slOptions.hudLessBufferFormat = options.hudLessBufferFormat;
    slOptions.uiBufferFormat = options.uiBufferFormat;
    slOptions.onErrorCallback = nullptr; // donut does not expose this

    sl::DLSSGState slState;
    successCheck(slDLSSGGetState(m_viewport, slState, &slOptions), "slDLSSGGetState");

    state.estimatedVRAMUsageInBytes = slState.estimatedVRAMUsageInBytes;
    switch (slState.status)
    {
    case (sl::DLSSGStatus::eOk)                                     : state.status = DLSSGStatus::eOk; break;
    case (sl::DLSSGStatus::eFailResolutionTooLow)                   : state.status = DLSSGStatus::eFailResolutionTooLow; break;
    case (sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)         : state.status = DLSSGStatus::eFailReflexNotDetectedAtRuntime; break;
    case (sl::DLSSGStatus::eFailHDRFormatNotSupported)              : state.status = DLSSGStatus::eFailHDRFormatNotSupported; break;
    case (sl::DLSSGStatus::eFailCommonConstantsInvalid)             : state.status = DLSSGStatus::eFailCommonConstantsInvalid; break;
    case (sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled) : state.status = DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled; break;
    default:
        log::warning("DLSSG status is unknown.");
        break;
    }
    state.minWidthOrHeight = slState.minWidthOrHeight;
    state.numFramesActuallyPresented = slState.numFramesActuallyPresented;
    state.numFramesToGenerateMax = slState.numFramesToGenerateMax;
    state.bIsVsyncSupportAvailable = slState.bIsVsyncSupportAvailable;
    state.inputsProcessingCompletionFence = slState.inputsProcessingCompletionFence;
    state.lastPresentInputsProcessingCompletionFenceValue = slState.lastPresentInputsProcessingCompletionFenceValue;
}

void StreamlineIntegration::CleanupDLSSG(bool wfi)
{
    if (!m_slInitialized)
    {
        log::warning("SL not initialised.");
        return;
    }

    if (!m_dlssgAvailable)
    {
        log::warning("DLSSG not available.");
        return;
    }

    if (wfi)
    {
        m_device->waitForIdle();
    }

    sl::Result status = slFreeResources(sl::kFeatureDLSS_G, m_viewport);
    // if we've never ran the feature on this viewport, this call may return 'eErrorInvalidParameter'
    assert(status == sl::Result::eOk || status == sl::Result::eErrorInvalidParameter);
}

sl::Resource StreamlineIntegration::AllocateResourceCallback(const sl::ResourceAllocationDesc* resDesc, void* device)
{
    sl::Resource res = {};

    if (device == nullptr)
    {
        log::warning("No device available for allocation.");
        return res;
    }

    bool isBuffer = (resDesc->type == sl::ResourceType::eBuffer);

    if (isBuffer)
    {
#if DONUT_WITH_DX11
        if (Get().m_api == nvrhi::GraphicsAPI::D3D11)
        {
            D3D11_BUFFER_DESC* desc = (D3D11_BUFFER_DESC*)resDesc->desc;
            ID3D11Device* pd3d11Device = (ID3D11Device*)device;
            ID3D11Buffer* pbuffer;
            bool success = SUCCEEDED(pd3d11Device->CreateBuffer(desc, nullptr, &pbuffer));
            if (!success) log::error("Failed to create buffer in SL allocation callback");
            res.type = resDesc->type;
            res.native = pbuffer;

        }
#endif

#if DONUT_WITH_DX12
        if (Get().m_api == nvrhi::GraphicsAPI::D3D12)
        {
            D3D12_RESOURCE_DESC* desc = (D3D12_RESOURCE_DESC*)resDesc->desc;
            D3D12_HEAP_PROPERTIES* heap = (D3D12_HEAP_PROPERTIES*)resDesc->heap;
            D3D12_RESOURCE_STATES state = (D3D12_RESOURCE_STATES)resDesc->state;
            ID3D12Device* pd3d12Device = (ID3D12Device*)device;
            ID3D12Resource* pbuffer;
            bool success = SUCCEEDED(pd3d12Device->CreateCommittedResource(heap, D3D12_HEAP_FLAG_NONE, desc, state, nullptr, IID_PPV_ARGS(&pbuffer)));
            if (!success) log::error("Failed to create buffer in SL allocation callback");
            res.type = resDesc->type;
            res.native = pbuffer;
        }
#endif
    }
    else
    {
#if DONUT_WITH_DX11
        if (Get().m_api == nvrhi::GraphicsAPI::D3D11)
        {
            D3D11_TEXTURE2D_DESC* desc = (D3D11_TEXTURE2D_DESC*)resDesc->desc;
            ID3D11Device* pd3d11Device = (ID3D11Device*)device;
            ID3D11Texture2D* ptexture;
            bool success = SUCCEEDED(pd3d11Device->CreateTexture2D(desc, nullptr, &ptexture));
            if (!success) log::error("Failed to create texture in SL allocation callback");
            res.type = resDesc->type;
            res.native = ptexture;

        }
#endif

#if DONUT_WITH_DX12
        if (Get().m_api == nvrhi::GraphicsAPI::D3D12)
        {
            D3D12_RESOURCE_DESC* desc = (D3D12_RESOURCE_DESC*)resDesc->desc;
            D3D12_RESOURCE_STATES state = (D3D12_RESOURCE_STATES)resDesc->state;
            D3D12_HEAP_PROPERTIES* heap = (D3D12_HEAP_PROPERTIES*)resDesc->heap;
            ID3D12Device* pd3d12Device = (ID3D12Device*)device;
            ID3D12Resource* ptexture;
            D3D12_CLEAR_VALUE* pClearValue = nullptr;
            D3D12_CLEAR_VALUE clearValue;
            // specify the clear value to avoid D3D warnings on ClearRenderTarget()
            if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
            {
                clearValue.Format = desc->Format;
                memset(clearValue.Color, 0, sizeof(clearValue.Color));
                pClearValue = &clearValue;
            }
            bool success = SUCCEEDED(pd3d12Device->CreateCommittedResource(heap, D3D12_HEAP_FLAG_NONE, desc, state, pClearValue, IID_PPV_ARGS(&ptexture)));
            if (!success) log::error("Failed to create texture in SL allocation callback");
            res.type = resDesc->type;
            res.native = ptexture;
        }
#endif
    }
    return res;
}

void StreamlineIntegration::ReleaseResourceCallback(sl::Resource* resource, void* device)
{
    if (resource)
    {
        auto i = (IUnknown*)resource->native;
        i->Release();
    }
};

#if DONUT_WITH_DX12
D3D12_RESOURCE_STATES D3D12convertResourceStates(nvrhi::ResourceStates stateBits)
{
    if (stateBits == nvrhi::ResourceStates::Common)
        return D3D12_RESOURCE_STATE_COMMON;

    D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON; // also 0

    if ((stateBits & nvrhi::ResourceStates::ConstantBuffer) != 0) result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if ((stateBits & nvrhi::ResourceStates::VertexBuffer) != 0) result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if ((stateBits & nvrhi::ResourceStates::IndexBuffer) != 0) result |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    if ((stateBits & nvrhi::ResourceStates::IndirectArgument) != 0) result |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if ((stateBits & nvrhi::ResourceStates::ShaderResource) != 0) result |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if ((stateBits & nvrhi::ResourceStates::UnorderedAccess) != 0) result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if ((stateBits & nvrhi::ResourceStates::RenderTarget) != 0) result |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if ((stateBits & nvrhi::ResourceStates::DepthWrite) != 0) result |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if ((stateBits & nvrhi::ResourceStates::DepthRead) != 0) result |= D3D12_RESOURCE_STATE_DEPTH_READ;
    if ((stateBits & nvrhi::ResourceStates::StreamOut) != 0) result |= D3D12_RESOURCE_STATE_STREAM_OUT;
    if ((stateBits & nvrhi::ResourceStates::CopyDest) != 0) result |= D3D12_RESOURCE_STATE_COPY_DEST;
    if ((stateBits & nvrhi::ResourceStates::CopySource) != 0) result |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if ((stateBits & nvrhi::ResourceStates::ResolveDest) != 0) result |= D3D12_RESOURCE_STATE_RESOLVE_DEST;
    if ((stateBits & nvrhi::ResourceStates::ResolveSource) != 0) result |= D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    if ((stateBits & nvrhi::ResourceStates::Present) != 0) result |= D3D12_RESOURCE_STATE_PRESENT;
    if ((stateBits & nvrhi::ResourceStates::AccelStructRead) != 0) result |= D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    if ((stateBits & nvrhi::ResourceStates::AccelStructWrite) != 0) result |= D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    if ((stateBits & nvrhi::ResourceStates::AccelStructBuildInput) != 0) result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if ((stateBits & nvrhi::ResourceStates::AccelStructBuildBlas) != 0) result |= D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    if ((stateBits & nvrhi::ResourceStates::ShadingRateSurface) != 0) result |= D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;

    return result;
}
#endif

nvrhi::Object StreamlineIntegration::GetNativeCommandList(nvrhi::ICommandList* commandList)
{
#if DONUT_WITH_DX11
    if (m_api == nvrhi::GraphicsAPI::D3D11)
        return m_device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
#endif

    if (commandList == nullptr)
    {
        log::error("Invalid command list!");
        return nullptr;
    }

#if DONUT_WITH_DX12
    if (m_api == nvrhi::GraphicsAPI::D3D12)
    {
        return commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
    }
#endif

#if DONUT_WITH_VULKAN
    if (m_api == nvrhi::GraphicsAPI::VULKAN)
    {
        return commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
    }
#endif

    return nullptr;
}

#if DONUT_WITH_VULKAN
static inline VkImageLayout toVkImageLayout(nvrhi::ResourceStates stateBits)
{
    switch (stateBits)
    {
    case nvrhi::ResourceStates::Common:
    case nvrhi::ResourceStates::UnorderedAccess: return VK_IMAGE_LAYOUT_GENERAL;
    case nvrhi::ResourceStates::ShaderResource: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case nvrhi::ResourceStates::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case nvrhi::ResourceStates::DepthWrite: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case nvrhi::ResourceStates::DepthRead: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    case nvrhi::ResourceStates::CopyDest:
    case nvrhi::ResourceStates::ResolveDest: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case nvrhi::ResourceStates::CopySource:
    case nvrhi::ResourceStates::ResolveSource: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case nvrhi::ResourceStates::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    default: return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}
#endif

static void GetSLResource(
    nvrhi::ICommandList* commandList,
    sl::Resource& slResource,
    nvrhi::ITexture* inputTex,
    const donut::engine::IView* view)
{
    if (commandList == nullptr)
    {
        log::error("Invalid command list!");
        return;
    }

    if (commandList->getDevice() == nullptr)
    {
        log::error("No device available.");
        return;
    }

    switch (commandList->getDevice()->getGraphicsAPI())
    {
#if DONUT_WITH_DX11
    case nvrhi::GraphicsAPI::D3D11:
        slResource = sl::Resource{ sl::ResourceType::eTex2d, inputTex->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource), 0 };
        break;
#endif

#if DONUT_WITH_DX12
    case nvrhi::GraphicsAPI::D3D12:
    {
        uint32_t resourceState = static_cast<uint32_t>(D3D12convertResourceStates(commandList->getTextureSubresourceState(inputTex, 0, 0)));
        slResource = sl::Resource{ sl::ResourceType::eTex2d, inputTex->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource), nullptr, nullptr, resourceState };
        break;
    }
#endif

#if DONUT_WITH_VULKAN
    case nvrhi::GraphicsAPI::VULKAN:
    {
        nvrhi::TextureSubresourceSet subresources = view->GetSubresources();
        auto const& desc = inputTex->getDesc();
        auto const vkDesc = static_cast<vk::ImageCreateInfo *>(inputTex->getNativeObject(nvrhi::ObjectTypes::VK_ImageCreateInfo));

        slResource = sl::Resource{ sl::ResourceType::eTex2d, inputTex->getNativeObject(nvrhi::ObjectTypes::VK_Image),
            inputTex->getNativeObject(nvrhi::ObjectTypes::VK_DeviceMemory),
            inputTex->getNativeView(nvrhi::ObjectTypes::VK_ImageView, desc.format, subresources),
            static_cast<uint32_t>(toVkImageLayout(desc.initialState)) };
        slResource.width = desc.width;
        slResource.height = desc.height;
        slResource.nativeFormat = static_cast<uint32_t>(nvrhi::vulkan::convertFormat(desc.format));
        slResource.mipLevels = desc.mipLevels;
        slResource.arrayLayers = vkDesc->arrayLayers;
        slResource.flags = static_cast<uint32_t>(vkDesc->flags);
        slResource.usage = static_cast<uint32_t>(vkDesc->usage);
    }
    break;
#endif

    default:
        log::error("Unsupported graphics API.");
        break;
    }
}

void StreamlineIntegration::TagResourcesGeneral(
    nvrhi::ICommandList* commandList,
    const donut::engine::IView* view,
    nvrhi::ITexture* motionVectors,
    nvrhi::ITexture* depth,
    nvrhi::ITexture* finalColorHudless)
{
    if (!m_slInitialized)
    {
        log::warning("Streamline not initialised.");
        return;
    }

    sl::Extent renderExtent{ 0, 0, depth->getDesc().width, depth->getDesc().height };
    sl::Extent fullExtent{ 0, 0, finalColorHudless->getDesc().width, finalColorHudless->getDesc().height };
    void* cmdbuffer = GetNativeCommandList(commandList);
    sl::Resource motionVectorsResource{}, depthResource{}, finalColorHudlessResource{};

    GetSLResource(commandList, motionVectorsResource, motionVectors, view);
    GetSLResource(commandList, depthResource, depth, view);
    GetSLResource(commandList, finalColorHudlessResource, finalColorHudless, view);

    sl::ResourceTag motionVectorsResourceTag = sl::ResourceTag{ &motionVectorsResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
    sl::ResourceTag depthResourceTag = sl::ResourceTag{ &depthResource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
    sl::ResourceTag finalColorHudlessResourceTag = sl::ResourceTag{ &finalColorHudlessResource, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

    sl::ResourceTag inputs[] = { motionVectorsResourceTag, depthResourceTag, finalColorHudlessResourceTag };
    successCheck(slSetTag(m_viewport, inputs, _countof(inputs), cmdbuffer), "slSetTag_General");
}

void StreamlineIntegration::TagResourcesDLSSNIS(
    nvrhi::ICommandList* commandList,
    const donut::engine::IView* view,
    nvrhi::ITexture* Output,
    nvrhi::ITexture* Input)
{
    if (!m_slInitialized)
    {
        log::warning("Streamline not initialised.");
        return;
    }

    sl::Extent renderExtent{ 0, 0, Input->getDesc().width, Input->getDesc().height };
    sl::Extent fullExtent{ 0, 0, Output->getDesc().width, Output->getDesc().height };
    void* cmdbuffer = GetNativeCommandList(commandList);
    sl::Resource outputResource{}, inputResource{};

    GetSLResource(commandList, outputResource, Output, view);
    GetSLResource(commandList, inputResource, Input, view);

    sl::ResourceTag inputResourceTag = sl::ResourceTag{ &inputResource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
    sl::ResourceTag outputResourceTag = sl::ResourceTag{ &outputResource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

    sl::ResourceTag inputs[] = { inputResourceTag, outputResourceTag };
    successCheck(slSetTag(m_viewport, inputs, _countof(inputs), cmdbuffer), "slSetTag_dlss_nis");
}

void StreamlineIntegration::TagResourcesDLSSFG(
    nvrhi::ICommandList* commandList,
    bool validViewportExtent,
    const Extent& backBufferExtent)
{
    if (!m_slInitialized)
    {
        log::warning("Streamline not initialised.");
        return;
    }

    void* cmdbuffer = GetNativeCommandList(commandList);

    // tag backbuffer resource mainly to pass extent data and therefore resource can be nullptr.
    // If the viewport extent is invalid - set extent to null. This informs streamline that full resource extent needs to be used
    sl::Extent slBackBufferExtent = { backBufferExtent.top, backBufferExtent.left, backBufferExtent.width, backBufferExtent.height };
    sl::ResourceTag backBufferResourceTag = sl::ResourceTag{ nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, validViewportExtent ? &slBackBufferExtent : nullptr };
    sl::ResourceTag inputs[] = { backBufferResourceTag };
    successCheck(slSetTag(m_viewport, inputs, _countof(inputs), cmdbuffer), "slSetTag_dlss_fg");
}

void StreamlineIntegration::TagResourcesDeepDVC(
    nvrhi::ICommandList* commandList,
    const donut::engine::IView* view,
    nvrhi::ITexture* Output)
{
    if (!m_slInitialized)
    {
        log::warning("Streamline not initialised.");
        return;
    }

    sl::Extent fullExtent{ 0, 0, Output->getDesc().width, Output->getDesc().height };
    void* cmdbuffer = GetNativeCommandList(commandList);
    sl::Resource outputResource{};

    GetSLResource(commandList, outputResource, Output, view);

    sl::ResourceTag outputResourceTag = sl::ResourceTag{ &outputResource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

    sl::ResourceTag inputs[] = { outputResourceTag };
    successCheck(slSetTag(m_viewport, inputs, _countof(inputs), cmdbuffer), "slSetTag_deepdvc");
}

void StreamlineIntegration::UnTagResourcesDeepDVC()
{
    sl::ResourceTag outputResourceTag = sl::ResourceTag{ nullptr, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent };

    sl::ResourceTag inputs[] = { outputResourceTag };
    successCheck(slSetTag(m_viewport, inputs, _countof(inputs), nullptr), "slSetTag_deepdvc_untag");
}

void StreamlineIntegration::TagResourcesDLSSRR(
    nvrhi::ICommandList* commandList,
    const donut::engine::IView* view,
    dm::int2 renderSize,
    dm::int2 displaySize,
    nvrhi::ITexture* inputColor,
    nvrhi::ITexture* diffuseAlbedo,
    nvrhi::ITexture* specAlbedo,
    nvrhi::ITexture* normalsAndOptionalRoughness,
    nvrhi::ITexture* roughness,
    nvrhi::ITexture* specHitDist,
    nvrhi::ITexture* specMotionVectors,
    nvrhi::ITexture* outputColor
)
{
    if (!m_slInitialized)
    {
        log::warning("Streamline not initialised.");
        return;
    }
    if (m_device == nullptr)
    {
        log::error("No device available.");
        return;
    }

    if ((specHitDist != nullptr && specMotionVectors != nullptr) || (specHitDist == nullptr && specMotionVectors == nullptr))
    {
        log::error("SpecHitDist and SpecMotionVectors are mutually exclusive - only one or the other can be used");
        return;
    }

#if STREAMLINE_HAS_DLSS_RR
    sl::Extent renderExtent{ 0, 0, inputColor->getDesc().width, inputColor->getDesc().height };
    sl::Extent fullExtent{ 0, 0, outputColor->getDesc().width, outputColor->getDesc().height };
    sl::Resource inputColorResource, diffuseAlbedoResource, specAlbedoResource, normalsAndOptionalRoughnessResource, roughnessResource, specHitDistResource, specMotionVectorsResource, outputColorResource;
    void* cmdbuffer = nullptr;

    GetSLResource(commandList, inputColorResource, inputColor, view);
    GetSLResource(commandList, diffuseAlbedoResource, diffuseAlbedo, view);
    GetSLResource(commandList, specAlbedoResource, specAlbedo, view);
    GetSLResource(commandList, normalsAndOptionalRoughnessResource, normalsAndOptionalRoughness, view);
    if (roughness != nullptr)
        GetSLResource(commandList, roughnessResource, roughness, view);
    if (specHitDist != nullptr)
        GetSLResource(commandList, specHitDistResource, specHitDist, view);
    if (specMotionVectors != nullptr)
        GetSLResource(commandList, specMotionVectorsResource, specMotionVectors, view);
    GetSLResource(commandList, outputColorResource, outputColor, view);

    if (m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        cmdbuffer = commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
    }
    else if (m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        cmdbuffer = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
    }

    // must match sl::DLSSDNormalRoughnessMode; if separate roughness provided then normalsAndOptionalRoughness is just normals, otherwise they're packed together with roughness in .w
    sl::BufferType normalsOrNormalsAndRoughnessBufferType = (roughness==nullptr)?(sl::kBufferTypeNormalRoughness):(sl::kBufferTypeNormals);

    sl::ResourceTag inputColorResourceTag = sl::ResourceTag{ &inputColorResource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
    sl::ResourceTag diffuseAlbedoResourceTag = sl::ResourceTag{ &diffuseAlbedoResource, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
    sl::ResourceTag specAlbedoResourceTag = sl::ResourceTag{ &specAlbedoResource, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
    sl::ResourceTag normalsAndOptionalRoughnessTag = sl::ResourceTag{ &normalsAndOptionalRoughnessResource, normalsOrNormalsAndRoughnessBufferType, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
    sl::ResourceTag roughnessResourceTag = sl::ResourceTag{ (roughness!=nullptr)?(&roughnessResource):(nullptr), sl::kBufferTypeRoughness, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
    sl::ResourceTag specHitDistResourceTag = sl::ResourceTag{ (specHitDist!=nullptr)?(&specHitDistResource):(nullptr), sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
    sl::ResourceTag specMotionVectorsResourceTag = sl::ResourceTag{ (specMotionVectors!=nullptr) ? (&specMotionVectorsResource) : (nullptr), sl::kBufferTypeSpecularMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
    sl::ResourceTag outputColorResourceTag = sl::ResourceTag{ &outputColorResource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

    sl::ResourceTag inputs[] = { inputColorResourceTag, diffuseAlbedoResourceTag, specAlbedoResourceTag, normalsAndOptionalRoughnessTag, roughnessResourceTag, specHitDistResourceTag, specMotionVectorsResourceTag, outputColorResourceTag };
    successCheck(slSetTag(m_viewport, inputs, _countof(inputs), cmdbuffer), "slSetTag_DLSSRR");
#endif
}

void StreamlineIntegration::EvaluateDLSS(nvrhi::ICommandList* commandList)
{
    void* nativeCommandList = GetNativeCommandList(commandList);
    if (nativeCommandList == nullptr)
    {
        log::warning("Failed to retrieve context for DLSS evaluation.");
        return;
    }

    sl::ViewportHandle view(m_viewport);
    const sl::BaseStructure* inputs[] = { &view };
    successCheck(slEvaluateFeature(sl::kFeatureDLSS, *m_currentFrame, inputs, _countof(inputs), nativeCommandList), "slEvaluateFeature_DLSS");

    //Our pipeline is very simple so we can simply clear it, but normally state tracking should be implemented.
    commandList->clearState();
}

void StreamlineIntegration::EvaluateDLSSRR(nvrhi::ICommandList* commandList)
{
#if STREAMLINE_HAS_DLSS_RR
    void* nativeCommandList = GetNativeCommandList(commandList);
    if (nativeCommandList == nullptr)
    {
        log::warning("Failed to retrieve context for DLSS evaluation.");
        return;
    }

    sl::ViewportHandle view(m_viewport);
    const sl::BaseStructure* inputs[] = { &view };
    successCheck(slEvaluateFeature(sl::kFeatureDLSS_RR, *m_currentFrame, inputs, _countof(inputs), nativeCommandList), "slEvaluateFeature_DLSS_RR");

    //Our pipeline is very simple so we can simply clear it, but normally state tracking should be implemented.
    commandList->clearState();
#endif
}

void StreamlineIntegration::EvaluateNIS(nvrhi::ICommandList* commandList)
{
    void* nativeCommandList = GetNativeCommandList(commandList);
    if (nativeCommandList == nullptr)
    {
        log::warning("Failed to retrieve context for NIS evaluation.");
        return;
    }

    sl::ViewportHandle view(m_viewport);
    const sl::BaseStructure* inputs[] = { &view };
    successCheck(slEvaluateFeature(sl::kFeatureNIS, *m_currentFrame, inputs, _countof(inputs), nativeCommandList), "slEvaluateFeature_NIS");

    //Our pipeline is very simple so we can simply clear it, but normally state tracking should be implemented.
    commandList->clearState();
}

void StreamlineIntegration::EvaluateDeepDVC(nvrhi::ICommandList* commandList)
{
    void* nativeCommandList = GetNativeCommandList(commandList);
    if (nativeCommandList == nullptr)
    {
        log::warning("Failed to retrieve context for NIS evaluation.");
        return;
    }

    sl::ViewportHandle view(m_viewport);
    const sl::BaseStructure* inputs[] = { &view };
    successCheck(slEvaluateFeature(sl::kFeatureDeepDVC, *m_currentFrame, inputs, _countof(inputs), nativeCommandList), "slEvaluateFeature_DeepDVC");

    //Our pipeline is very simple so we can simply clear it, but normally state tracking should be implemented.
    commandList->clearState();
}

void StreamlineIntegration::QueryDeepDVCState(uint64_t& estimatedVRamUsage)
{
    if (!m_slInitialized || !m_deepdvcAvailable)
    {
        log::warning("SL not initialised or DeepDVC not available.");
        return;
    }
    sl::DeepDVCState state;
    successCheck(slDeepDVCGetState(m_viewport, state), "slDeepDVCGetState");
    estimatedVRamUsage = state.estimatedVRAMUsageInBytes;
}

void StreamlineIntegration::SetReflexConsts(const ReflexOptions& options)
{
    static_assert(sl::ReflexMode::eOff == (sl::ReflexMode)StreamlineIntegration::ReflexMode::eOff);
    static_assert(sl::ReflexMode::eLowLatency == (sl::ReflexMode)StreamlineIntegration::ReflexMode::eLowLatency);
    static_assert(sl::ReflexMode::eLowLatencyWithBoost == (sl::ReflexMode)StreamlineIntegration::ReflexMode::eLowLatencyWithBoost);

#if (SL_VERSION_MAJOR >= 2 && (SL_VERSION_MAJOR > 2 || SL_VERSION_MINOR >= 7))
    static_assert(sl::ReflexMode::ReflexMode_eCount == (sl::ReflexMode)StreamlineIntegration::ReflexMode::ReflexMode_eCount);
#endif

    sl::ReflexOptions slOptions;
    slOptions.mode = (sl::ReflexMode)options.mode;
    slOptions.frameLimitUs = options.frameLimitUs;
    slOptions.useMarkersToOptimize = options.useMarkersToOptimize;
    slOptions.virtualKey = options.virtualKey;
    slOptions.idThread = options.idThread;

    if (!m_slInitialized || !m_reflexAvailable)
    {
        log::warning("SL not initialised or Reflex not available.");
        return;
    }

    successCheck(slReflexSetOptions(slOptions), "slReflexSetOptions");
}

void StreamlineIntegration::GetReflexState(ReflexState& state) const
{
    if (!m_slInitialized || !m_reflexAvailable)
    {
        log::warning("SL not initialised or Reflex not available.");
        return;
    }

    sl::ReflexState slState;
    successCheck(slReflexGetState(slState), "slReflexGetState");
    state.lowLatencyAvailable = slState.lowLatencyAvailable;
    state.latencyReportAvailable = slState.latencyReportAvailable;
    state.statsWindowMessage = slState.statsWindowMessage;
    state.flashIndicatorDriverControlled = slState.flashIndicatorDriverControlled;
    for (size_t i = 0; i < 64; i++)
    {
        state.frameReport[i].frameID                = slState.frameReport[i].frameID;
        state.frameReport[i].inputSampleTime        = slState.frameReport[i].inputSampleTime;
        state.frameReport[i].simStartTime           = slState.frameReport[i].simStartTime;
        state.frameReport[i].simEndTime             = slState.frameReport[i].simEndTime;
        state.frameReport[i].renderSubmitStartTime  = slState.frameReport[i].renderSubmitStartTime;
        state.frameReport[i].renderSubmitEndTime    = slState.frameReport[i].renderSubmitEndTime;
        state.frameReport[i].presentStartTime       = slState.frameReport[i].presentStartTime;
        state.frameReport[i].presentEndTime         = slState.frameReport[i].presentEndTime;
        state.frameReport[i].driverStartTime        = slState.frameReport[i].driverStartTime;
        state.frameReport[i].driverEndTime          = slState.frameReport[i].driverEndTime;
        state.frameReport[i].osRenderQueueStartTime = slState.frameReport[i].osRenderQueueStartTime;
        state.frameReport[i].osRenderQueueEndTime   = slState.frameReport[i].osRenderQueueEndTime;
        state.frameReport[i].gpuRenderStartTime     = slState.frameReport[i].gpuRenderStartTime;
        state.frameReport[i].gpuRenderEndTime       = slState.frameReport[i].gpuRenderEndTime;
        state.frameReport[i].gpuActiveRenderTimeUs  = slState.frameReport[i].gpuActiveRenderTimeUs;
        state.frameReport[i].gpuFrameTimeUs         = slState.frameReport[i].gpuFrameTimeUs;
    }
}

void StreamlineIntegration::SimStart(DeviceManager& manager)
{
    successCheck(slGetNewFrameToken(m_currentFrame, nullptr), "SL_GetFrameToken");

    if (IsReflexAvailable())
    {
        successCheck(slReflexSleep(*m_currentFrame), "Reflex_Sleep");

    }
    if (IsPCLAvailable())
    {
        successCheck(slPCLSetMarker(sl::PCLMarker::eSimulationStart, *m_currentFrame), "PCL_BeforeFrame");
    }
}

void StreamlineIntegration::SimEnd(DeviceManager& manager)
{
    if (IsPCLAvailable())
    {
        successCheck(slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *m_currentFrame), "PCL_SimEnd");
    }
}

void StreamlineIntegration::RenderStart(DeviceManager& manager)
{
    if (IsPCLAvailable())
    {
        successCheck(slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *m_currentFrame), "PCL_SubmitStart");
    }
}

void StreamlineIntegration::RenderEnd(DeviceManager& manager)
{
    if (IsPCLAvailable())
    {
        successCheck(slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *m_currentFrame), "PCL_SubmitEnd");
    }
}

void StreamlineIntegration::PresentStart(DeviceManager& manager)
{
    if (IsPCLAvailable())
    {
        successCheck(slPCLSetMarker(sl::PCLMarker::ePresentStart, *m_currentFrame), "PCL_PresentStart");
    }
}

void StreamlineIntegration::PresentEnd(DeviceManager& manager)
{
    if (IsPCLAvailable())
    {
        successCheck(slPCLSetMarker(sl::PCLMarker::ePresentEnd, *m_currentFrame), "PCL_PresentEnd");
    }
}

void StreamlineIntegration::ReflexTriggerFlash(int frameNumber)
{
    successCheck(slPCLSetMarker(sl::PCLMarker::eTriggerFlash, *m_currentFrame), "Reflex_Flash");
}

void StreamlineIntegration::ReflexTriggerPcPing(int frameNumber)
{
    if (IsPCLAvailable())
    {
        successCheck(slPCLSetMarker(sl::PCLMarker::ePCLatencyPing, *m_currentFrame), "PCL_PCPing");
    }
}

} // namespace donut::app

#endif
