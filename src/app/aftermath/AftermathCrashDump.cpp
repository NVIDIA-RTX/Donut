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

#include <sstream>

#include <donut/core/log.h>
#include <donut/app/AftermathCrashDump.h>
#include "donut/app/ApplicationBase.h"

#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

static void DumpFileCallback(const void* pGpuCrashDump, const uint32_t gpuCrashDumpSize, void* pUserData)
{
    donut::app::AftermathCrashDump *dumper = reinterpret_cast<donut::app::AftermathCrashDump*>(pUserData);
    std::filesystem::create_directory(dumper->GetDumpFolder());

    auto nativeFS = std::make_unique<donut::vfs::NativeFileSystem>();
    std::filesystem::path dumpPath = dumper->GetDumpFolder() / "crash.nv-gpudmp";
    nativeFS->writeFile(dumpPath, pGpuCrashDump, gpuCrashDumpSize);

    // Surface the absolute dump path so post-mortem tooling (e.g. the
    // nv-aftermath skill / nv-aftermath-format.exe) can pick it up
    // straight from the log without grepping.
    donut::log::info("Aftermath crash dump written: %s",
                     std::filesystem::absolute(dumpPath).string().c_str());

    GFSDK_Aftermath_GpuCrashDump_Decoder decoder = {};
    GFSDK_Aftermath_Result result = GFSDK_Aftermath_GpuCrashDump_CreateDecoder(GFSDK_Aftermath_Version_API, pGpuCrashDump, gpuCrashDumpSize, &decoder);
    if (!GFSDK_Aftermath_SUCCEED(result))
    {
        donut::log::error("Aftermath crash dump decoder failed create with error 0x%.8x", result);
        return;
    }
    uint32_t numActiveShaders = 0;
    GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfoCount(decoder, &numActiveShaders);
    if (numActiveShaders > 0)
    {
        std::vector<GFSDK_Aftermath_GpuCrashDump_ShaderInfo> shaderInfos{ numActiveShaders };
        GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfo(decoder, numActiveShaders, shaderInfos.data());
        for (auto& shaderInfo : shaderInfos)
        {
            if (!shaderInfo.isInternal)
            {
                GFSDK_Aftermath_ShaderBinaryHash shaderHash = {};
                GFSDK_Aftermath_GetShaderHashForShaderInfo(decoder, &shaderInfo, &shaderHash);
                nvrhi::AftermathCrashDumpHelper& crashDumpHelper = dumper->GetDeviceManager().GetDevice()->getAftermathCrashDumpHelper();
                nvrhi::BinaryBlob shaderLookupResult = crashDumpHelper.findShaderBinary(shaderHash.hash, donut::app::AftermathCrashDump::GetShaderHashForBinary);
                if (shaderLookupResult.second > 0)
                {
                    std::stringstream ss;
                    ss << std::hex << shaderHash.hash << ".bin";
                    std::filesystem::path shaderPath = dumper->GetDumpFolder() / ss.str();
                    nativeFS->writeFile(shaderPath, shaderLookupResult.first, shaderLookupResult.second);
                }
            }
        }
    }
    GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
}

static void ShaderDebugInfoCallback(const void* pShaderDebugInfo, const uint32_t shaderDebugInfoSize, void* pUserData)
{
    donut::app::AftermathCrashDump* dumper = reinterpret_cast<donut::app::AftermathCrashDump*>(pUserData);
    std::filesystem::create_directory(dumper->GetDumpFolder());

    auto nativeFS = std::make_unique<donut::vfs::NativeFileSystem>();
    // the hash used for nsight is stored in the shader debug info file in address 0x20-0x40
    // the name (in terms of uint64s at byte addresses) is 0x28 0x20 - 0x38 0x30
    // which in terms of uint64 addresses is 0x5 0x4 - 0x7 0x6
    const uint64_t* ptr64 = reinterpret_cast<const uint64_t*>(pShaderDebugInfo);
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(8) << ptr64[5] << std::setw(8) << ptr64[4]
        << "-" << std::setw(8) << ptr64[7] << std::setw(8) << ptr64[6] << ".nvdbg";
    std::filesystem::path dumpPath = dumper->GetDumpFolder() / ss.str();
    nativeFS->writeFile(dumpPath, pShaderDebugInfo, shaderDebugInfoSize);
}

static void DescriptionCallback(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDescription, void* pUserData)
{
    donut::app::AftermathCrashDump* dumper = reinterpret_cast<donut::app::AftermathCrashDump*>(pUserData);
    addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, dumper->GetDeviceManager().GetWindowTitle());
}

// this callback should call into the nvrhi device which has the necessary information
static void ResolveMarkerCallback(const void* pMarkerData, const uint32_t markerDataSize, void* pUserData, void** ppResolvedMarkerData, uint32_t* pResolvedMarkerDataSize)
{
    donut::app::AftermathCrashDump* dumper = reinterpret_cast<donut::app::AftermathCrashDump*>(pUserData);
    const uint64_t markerAsHash = reinterpret_cast<const uint64_t>(pMarkerData);
    // as long as the device is not yet destroyed, these references should be ok to pass back
    const std::string& resolvedMarker = dumper->ResolveMarker(markerAsHash);
    *ppResolvedMarkerData = (void*) resolvedMarker.data();
    *pResolvedMarkerDataSize = uint32_t(resolvedMarker.length());
}

void donut::app::AftermathCrashDump::WaitForCrashDump(uint32_t maxTimeoutSeconds)
{
    std::chrono::time_point startTime = std::chrono::system_clock::now();
    bool timedOut = false;
    while (!timedOut)
    {
        GFSDK_Aftermath_CrashDump_Status crashDumpStatus = GFSDK_Aftermath_CrashDump_Status_Unknown;
        GFSDK_Aftermath_GetCrashDumpStatus(&crashDumpStatus);
        if (crashDumpStatus == GFSDK_Aftermath_CrashDump_Status_Finished)
        {
            break;
        }
        auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - startTime).count();
        timedOut = elapsedSeconds > maxTimeoutSeconds;
    }
}

uint64_t donut::app::AftermathCrashDump::GetShaderHashForBinary(std::pair<const void*, size_t> shaderBinary, nvrhi::GraphicsAPI api)
{
#if DONUT_WITH_VULKAN
    if (api == nvrhi::GraphicsAPI::VULKAN)
    {
        GFSDK_Aftermath_SpirvCode spirv = {};
        spirv.pData = shaderBinary.first;
        spirv.size = uint32_t(shaderBinary.second);
        GFSDK_Aftermath_ShaderBinaryHash hash = {};
        GFSDK_Aftermath_GetShaderHashSpirv(GFSDK_Aftermath_Version_API, &spirv, &hash);
        return hash.hash;
    }
#endif
#if DONUT_WITH_DX11 || DONUT_WITH_DX12
    if (api == nvrhi::GraphicsAPI::D3D11 || api == nvrhi::GraphicsAPI::D3D12)
    {
        D3D12_SHADER_BYTECODE dxil = {};
        dxil.pShaderBytecode = shaderBinary.first;
        dxil.BytecodeLength = shaderBinary.second;
        GFSDK_Aftermath_ShaderBinaryHash hash = {};
        GFSDK_Aftermath_GetShaderHash(GFSDK_Aftermath_Version_API, &dxil, &hash);
        return hash.hash;
    }
#endif
    return 0;
}

static bool AftermathInitialized = false;
void donut::app::AftermathCrashDump::InitializeAftermathCrashDump(AftermathCrashDump* dumper)
{
    // if already initialized, reinit with new crash dumper
    if (AftermathInitialized)
    {
        GFSDK_Aftermath_DisableGpuCrashDumps();
    }

    uint32_t watchedApis = GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_None;
#if DONUT_WITH_DX11 || DONUT_WITH_DX12
    watchedApis |= GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_DX;
#endif
#if DONUT_WITH_VULKAN
    watchedApis |= GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan;
#endif
    uint32_t featureFlags = GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks;
    GFSDK_Aftermath_Result result = GFSDK_Aftermath_EnableGpuCrashDumps(
        GFSDK_Aftermath_Version_API,
        watchedApis,
        featureFlags,
        DumpFileCallback,
        ShaderDebugInfoCallback,
        DescriptionCallback,
        ResolveMarkerCallback,
        dumper);
    if (!GFSDK_Aftermath_SUCCEED(result))
    {
        log::error("Aftermath crash dump enable failed with error 0x%.8x", result);
    }
}


donut::app::AftermathCrashDump::AftermathCrashDump(DeviceManager& deviceManager) :
    m_deviceManager(deviceManager)
{
}

void donut::app::AftermathCrashDump::EnableCrashDumpTracking()
{
    InitializeAftermathCrashDump(this);
    // create a unique path to store the dump files based on date/time
    // using date/time at creation time since we need to use the same value for different callbacks but they will be called at different times
    std::stringstream folder;
    std::time_t t = std::time(nullptr);
    std::tm tm = *std::localtime(&t);
    folder << "crash_" << std::put_time(&tm, "%Y-%m-%d-%H_%M_%S");
    m_dumpFolder = app::GetDirectoryWithExecutable() / folder.str();
}

const std::string& donut::app::AftermathCrashDump::ResolveMarker(uint64_t markerHash)
{
    auto [found, markerString] = m_deviceManager.GetDevice()->getAftermathCrashDumpHelper().ResolveMarker(markerHash);
    return markerString;
}

donut::app::DeviceManager& donut::app::AftermathCrashDump::GetDeviceManager()
{
    return m_deviceManager;
}

std::filesystem::path donut::app::AftermathCrashDump::GetDumpFolder()
{
    return m_dumpFolder;
}
