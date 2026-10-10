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
#pragma once

// Donut
#include <donut/engine/View.h>
#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

namespace donut::app 
{
class DeviceManager;

// Outside the class: GCC rejects a nested type's member initializers in a
// default argument of the still-incomplete enclosing class (GCC bug 88165).
struct StreamlineExtent
{
    uint32_t top = 0u;
    uint32_t left = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
};

// Declared on every platform so cross-platform code compiles where Streamline is
// unsupported; an instance exists only via DeviceManager::GetStreamline() when enabled.
class StreamlineInterface
{
public:
    static constexpr float kInvalidFloat = 3.40282346638528859811704183484516925440e38f;
    static constexpr uint32_t kInvalidUint = 0xffffffff;
    using Extent = StreamlineExtent;

    // Set the current viewport which affects constants, options and tagging
    virtual void SetViewport(uint32_t viewportIndex) = 0;

    // see sl_consts.h for documentation
    struct Constants 
    {
        dm::float4x4 cameraViewToClip;
        dm::float4x4 clipToCameraView;
        dm::float4x4 clipToLensClip;
        dm::float4x4 clipToPrevClip;
        dm::float4x4 prevClipToClip;

        dm::float2 jitterOffset;
        dm::float2 mvecScale;
        dm::float2 cameraPinholeOffset;
        dm::float3 cameraPos;
        dm::float3 cameraUp;
        dm::float3 cameraRight;
        dm::float3 cameraFwd;

        float cameraNear = kInvalidFloat;
        float cameraFar = kInvalidFloat;
        float cameraFOV = kInvalidFloat;
        float cameraAspectRatio = kInvalidFloat;
        float motionVectorsInvalidValue = kInvalidFloat;

        bool depthInverted = false;
        bool cameraMotionIncluded = false;
        bool motionVectors3D = false;
        bool reset = false;
        bool orthographicProjection = false;        
        bool motionVectorsDilated = false;
        bool motionVectorsJittered = false;
        float minRelativeLinearDepthObjectSeparation = 40.0f;
    };
    virtual void SetConstants(const Constants& consts) = 0;

    // See sl_dlss.h for documentation
    enum class DLSSMode : uint32_t
    {
        eOff,
        eMaxPerformance,
        eBalanced,
        eMaxQuality,
        eUltraPerformance,
        eUltraQuality,
        eDLAA,
        eCount,
    };
    enum class DLSSPreset : uint32_t
    {
        eDefault,
        ePresetA,
        ePresetB,
        ePresetC,
        ePresetD,
        ePresetE,
        ePresetF,
        ePresetG,
        ePresetH,
        ePresetI,
        ePresetJ,
        ePresetK,
    };
    struct DLSSOptions
    {
        DLSSMode mode = DLSSMode::eOff;
        uint32_t outputWidth = kInvalidUint;
        uint32_t outputHeight = kInvalidUint;
        float sharpness = 0.0f;
        float preExposure = 1.0f;
        float exposureScale = 1.0f;
        bool colorBuffersHDR = true;
        bool indicatorInvertAxisX = false;
        bool indicatorInvertAxisY = false;
        DLSSPreset preset = DLSSPreset::eDefault;
        bool useAutoExposure = false;
        bool alphaUpscalingEnabled = false;
    };
    struct DLSSSettings
    {
        dm::int2 optimalRenderSize;
        dm::int2 minRenderSize;
        dm::int2 maxRenderSize;
        float sharpness;
    };
    virtual void SetDLSSOptions(const DLSSOptions& options) = 0;
    virtual bool IsDLSSAvailable() const = 0;
    virtual void QueryDLSSOptimalSettings(const DLSSOptions& options, DLSSSettings& settings) = 0;
    virtual void EvaluateDLSS(nvrhi::ICommandList* commandList) = 0;
    virtual void CleanupDLSS(bool wfi) = 0;

    // See sl_nis.h for documentation
    enum class NISMode : uint32_t
    {
        eOff,
        eScaler,
        eSharpen,
        eCount
    };
    enum class NISHDR : uint32_t
    {
        eNone,
        eLinear,
        ePQ,
        eCount
    };
    struct NISOptions
    {
        NISMode mode = NISMode::eScaler;
        NISHDR hdrMode = NISHDR::eNone;
        float sharpness = 0.0f;
    };
    virtual void SetNISOptions(const NISOptions& options) = 0;
    virtual bool IsNISAvailable() const = 0;
    virtual void EvaluateNIS(nvrhi::ICommandList* commandList) = 0;
    virtual void CleanupNIS(bool wfi) = 0;

    // See sl_dvc.h for documentation
    enum class DeepDVCMode : uint32_t
    {
        eOff,
        eOn,
        eCount
    };
    struct DeepDVCOptions
    {
        DeepDVCMode mode = DeepDVCMode::eOff;
        float intensity = 0.5f;
        float saturationBoost = 0.25f;
    };
    virtual void SetDeepDVCOptions(const DeepDVCOptions& options) = 0;
    virtual bool IsDeepDVCAvailable() const = 0;
    virtual void QueryDeepDVCState(uint64_t& estimatedVRamUsage) = 0;
    virtual void EvaluateDeepDVC(nvrhi::ICommandList* commandList) = 0;
    virtual void CleanupDeepDVC() = 0;

    // See sl_reflex.h for documentation
    enum ReflexMode
    {
        eOff,
        eLowLatency,
        eLowLatencyWithBoost,
        ReflexMode_eCount
    };
    struct ReflexOptions
    {
        ReflexMode mode = ReflexMode::eOff;
        uint32_t frameLimitUs = 0;
        bool useMarkersToOptimize = false;
        uint16_t virtualKey = 0;
        uint32_t idThread = 0;
    };
    struct ReflexReport
    {
        uint64_t frameID{};
        uint64_t inputSampleTime{};
        uint64_t simStartTime{};
        uint64_t simEndTime{};
        uint64_t renderSubmitStartTime{};
        uint64_t renderSubmitEndTime{};
        uint64_t presentStartTime{};
        uint64_t presentEndTime{};
        uint64_t driverStartTime{};
        uint64_t driverEndTime{};
        uint64_t osRenderQueueStartTime{};
        uint64_t osRenderQueueEndTime{};
        uint64_t gpuRenderStartTime{};
        uint64_t gpuRenderEndTime{};
        uint32_t gpuActiveRenderTimeUs{};
        uint32_t gpuFrameTimeUs{};
    };
    struct ReflexState
    {
        bool lowLatencyAvailable = false;
        bool latencyReportAvailable = false;
        uint32_t statsWindowMessage;
        ReflexReport frameReport[64];
        bool flashIndicatorDriverControlled = false;
    };
    virtual void GetReflexState(ReflexState& state) const = 0;
    virtual bool IsReflexAvailable() const = 0;
    virtual bool IsPCLAvailable() const = 0;
    virtual void SetReflexConsts(const ReflexOptions& options) = 0;

    virtual void ReflexTriggerFlash(int frameNumber) = 0;
    virtual void ReflexTriggerPcPing(int frameNumber) = 0;
    
    // See dlss_g.h for documentation
    enum class DLSSGMode : uint32_t
    {
        eOff,
        eOn,
        eAuto,
        eCount
    };
    enum class DLSSGFlags : uint32_t
    {
        eShowOnlyInterpolatedFrame = 1 << 0,
        eDynamicResolutionEnabled = 1 << 1,
        eRequestVRAMEstimate = 1 << 2,
        eRetainResourcesWhenOff = 1 << 3,
        eEnableFullscreenMenuDetection = 1 << 4,
    };
    enum class DLSSGQueueParallelismMode : uint32_t
    {
        eBlockPresentingClientQueue,
        eBlockNoClientQueues,
        eCount
    };
    struct DLSSGOptions
    {
        DLSSGMode mode = DLSSGMode::eOff;
        uint32_t numFramesToGenerate = 1;
        DLSSGFlags flags{};
        uint32_t dynamicResWidth{};
        uint32_t dynamicResHeight{};
        uint32_t numBackBuffers{};
        uint32_t mvecDepthWidth{};
        uint32_t mvecDepthHeight{};
        uint32_t colorWidth{};
        uint32_t colorHeight{};
        uint32_t colorBufferFormat{};
        uint32_t mvecBufferFormat{};
        uint32_t depthBufferFormat{};
        uint32_t hudLessBufferFormat{};
        uint32_t uiBufferFormat{};
        bool useReflexMatrices = false;
        DLSSGQueueParallelismMode queueParallelismMode{};
    };
    enum class DLSSGStatus : uint32_t
    {
        eOk = 0,
        eFailResolutionTooLow = 1 << 0,
        eFailReflexNotDetectedAtRuntime = 1 << 1,
        eFailHDRFormatNotSupported = 1 << 2,
        eFailCommonConstantsInvalid = 1 << 3,
        eFailGetCurrentBackBufferIndexNotCalled = 1 << 4
    };
    struct DLSSGState
    {
        uint64_t estimatedVRAMUsageInBytes{};
        DLSSGStatus status{};
        uint32_t minWidthOrHeight{};
        uint32_t numFramesActuallyPresented{};
        uint32_t numFramesToGenerateMax{};
        bool bIsVsyncSupportAvailable{};
        void* inputsProcessingCompletionFence{};
        uint64_t lastPresentInputsProcessingCompletionFenceValue{};
    };
    virtual void GetDLSSGState(DLSSGState& state, const DLSSGOptions& options) = 0;
    virtual void SetDLSSGOptions(const DLSSGOptions& options) = 0;
    virtual bool IsDLSSGAvailable() const = 0;
    virtual void CleanupDLSSG(bool wfi) = 0;

    // See dlss_d.h for documentation
    enum class DLSSRRPreset : uint32_t
    {
        eDefault,
        ePresetA,
        ePresetB,
        ePresetC,
        ePresetD,
        ePresetE,
        ePresetF,
        ePresetG,
        ePresetH,
    };
    enum class DLSSRRNormalRoughnessMode : uint32_t
    {
        eUnpacked,  // App needs to provide Normal resource and Roughness resource separately.
        ePacked,    // App needs to write Roughness to w channel of Normal resource.
    };
    struct DLSSRROptions
    {
        DLSSMode mode = DLSSMode::eOff;
        uint32_t outputWidth = kInvalidUint;
        uint32_t outputHeight = kInvalidUint;
        float sharpness = 0.0f;
        float preExposure = 1.0f;
        float exposureScale = 1.0f;
        bool colorBuffersHDR = true;
        bool indicatorInvertAxisX = false;
        bool indicatorInvertAxisY = false;
        DLSSRRNormalRoughnessMode normalRoughnessMode = DLSSRRNormalRoughnessMode::eUnpacked;
        dm::float4x4 worldToCameraView;
        dm::float4x4 cameraViewToWorld;
        bool alphaUpscalingEnabled = false;

        DLSSRRPreset preset = DLSSRRPreset::eDefault;
    };
    struct DLSSRRSettings
    {
        donut::math::int2 optimalRenderSize;
        donut::math::int2 minRenderSize;
        donut::math::int2 maxRenderSize;
        float sharpness;
    };
    virtual void SetDLSSRROptions(const DLSSRROptions& options) = 0;
    virtual bool IsDLSSRRAvailable() const = 0;
    virtual void QueryDLSSRROptimalSettings(const DLSSRROptions& options, DLSSRRSettings& settings) = 0;
    virtual void EvaluateDLSSRR(nvrhi::ICommandList* commandList) = 0;

    virtual void TagResourcesGeneral(
        nvrhi::ICommandList* commandList,
        const donut::engine::IView* view,
        nvrhi::ITexture* motionVectors,
        nvrhi::ITexture* depth,
        nvrhi::ITexture* finalColorHudless) = 0;

    virtual void TagResourcesDLSSNIS(
        nvrhi::ICommandList* commandList,
        const donut::engine::IView* view,
        nvrhi::ITexture* output,
        nvrhi::ITexture* input) = 0;

    virtual void TagResourcesDLSSFG(
        nvrhi::ICommandList* commandList,
        bool validViewportExtent = false,
        const Extent& backBufferExtent = {}) = 0;

    virtual void TagResourcesDeepDVC(
        nvrhi::ICommandList* commandList,
        const donut::engine::IView* view,
        nvrhi::ITexture* output) = 0;

    virtual void UnTagResourcesDeepDVC() = 0;

	// * If roughness != nullptr, normalsAndOptionalRoughness contains only normals. If roughness == nullptr then the 
	//   roughness value should be in .a channel of the normalsAndOptionalRoughness and normalRoughnessMode in 
	//   StreamlineInterface::DLSSRROptions must be set to DLSSRRNormalRoughnessMode::ePacked.
	// * Either specHitDist or specMotionVectors should be provided but not both nor neither. Refer to DLSS-RR 
	//   documentation for more detail.
    virtual void TagResourcesDLSSRR(
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
    ) = 0;

    virtual void SimStart(DeviceManager& manager) = 0;
    virtual void SimEnd(DeviceManager& manager) = 0;
    virtual void RenderStart(DeviceManager& manager) = 0;
    virtual void RenderEnd(DeviceManager& manager) = 0;
    virtual void PresentStart(DeviceManager& manager) = 0;
    virtual void PresentEnd(DeviceManager& manager) = 0;
};


} // namespace donut::app