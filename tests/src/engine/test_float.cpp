/*
* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
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

#if DONUT_WITH_DX12
#include <directx/d3d12.h>
extern "C"
{
    _declspec(dllexport) extern const unsigned int D3D12SDKVersion = D3D12_PREVIEW_SDK_VERSION;
    _declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\";
}
#endif

#include <donut/core/math/float.h>
#include <donut/core/log.h>
#include <donut/tests/utils.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/ApplicationBase.h>
#include <cmath>
#include <memory>
#include <random>
#include <nvrhi/utils.h>

using namespace donut::math;

constexpr uint32_t MAX_ERRORS = 16;

bool check_known_float16(float in, uint16_t out)
{
    float16_t f16o = Float32ToFloat16(in);
    float f32o = Float16ToFloat32(f16o);

    bool pass = f16o.bits == out;
    if (std::isinf(in))
        pass = pass && isinf(f16o) && std::isinf(f32o);
    else if (std::isnan(in))
        pass = isnan(f16o) && std::isnan(f32o); // Exact NaN mantissa bits are allowed to mismatch
    
    if (!pass)
    {
        fprintf(stderr, "Known FLOAT16 mismatch: expected 0x%04x, got 0x%04x, float value %f\n",
            out, f16o.bits, in);
    }
    return pass;
}

bool test_known_float16()
{
    // See https://en.wikipedia.org/wiki/Half-precision_floating-point_format#Half_precision_examples
    const float inv1024 = 1.f / 1024.f;
    const float smallestNormal = powf(2.f, -14.f);

    using f32lim = std::numeric_limits<float>;
    using f16lim = std::numeric_limits<float16_t>;

    bool pass = true;
    pass &= check_known_float16(0.f, 0);
    pass &= check_known_float16(smallestNormal * inv1024 * 0.5f, 0); // underflow
    pass &= check_known_float16(smallestNormal * inv1024, f16lim::denorm_min().bits);
    pass &= check_known_float16(smallestNormal * inv1024 * 1023.f, 0x03ff);
    pass &= check_known_float16(1.f / 3.f, 0x3555);
    pass &= check_known_float16(0.5f * (1.f + 1023.f / 1024.f), 0x3bff);
    pass &= check_known_float16(1.f, 0x3c00);
    pass &= check_known_float16(1.f + inv1024, 0x3c01);
    pass &= check_known_float16(65504.f, f16lim::max().bits); // largest finite number
    pass &= check_known_float16(f32lim::infinity(), f16lim::infinity().bits);
    pass &= check_known_float16(-f32lim::infinity(), 0xfc00);
    pass &= check_known_float16(f32lim::quiet_NaN(), f16lim::quiet_NaN().bits);
    pass &= check_known_float16(f32lim::signaling_NaN(), f16lim::quiet_NaN().bits); // no signaling NaN here
    pass &= check_known_float16(65519.f, f16lim::max().bits); // not quite overflow to Inf
    pass &= check_known_float16(65520.f, f16lim::infinity().bits); // overflow to Inf
    pass &= check_known_float16(1000000.f, f16lim::infinity().bits); // overflow to Inf
    return pass;
}

bool test_float16_vectors()
{
    // This test just checks if packing and shuffling works correctly,
    // it doesn't need to cover various possible values.

    float2 f32_2(1.f, -2.f); // negative [1] to test the upper (sign) bit in the int32 packed version
    float16_t2 f16_2 = Float32ToFloat16x2(f32_2);
    float2 f32_2_out = Float16ToFloat32x2(f16_2);

    bool pass = true;
    for (int i = 0; i < 2; ++i)
    {
        if (f32_2_out[i] != f32_2[i])
        {
            fprintf(stderr, "FLOAT16x2 [%d] mismatch: expected %f, got %f\n", i, f32_2[i], f32_2_out[i]);
            pass = false;
        }
    }

    float4 f32_4(1.f, -2.f, 3.f, -4.f); // negative [3] to test the upper (sign) bit in the int64 packed version
    float16_t4 f16_4 = Float32ToFloat16x4(f32_4);
    float4 f32_4_out = Float16ToFloat32x4(f16_4);

    for (int i = 0; i < 4; ++i)
    {
        if (f32_4_out[i] != f32_4[i])
        {
            fprintf(stderr, "FLOAT16x4 [%d] mismatch: expected %f, got %f\n", i, f32_4[i], f32_4_out[i]);
            pass = false;
        }
    }

    return pass;
}

bool test_float16()
{
    float16_t f16i;
    float16_t f16o;
    float f32;
    uint32_t errorCount = 0;
    for (int i = 0; i < 65536; ++i)
    {
        f16i.bits = i;
        f32 = Float16ToFloat32(f16i);
        f16o = Float32ToFloat16(f32);

        bool pass;
        if (isinf(f16i))
            pass = std::isinf(f32) == isinf(f16o) && signbit(f16i) == signbit(f16o);
        else if (isnan(f16i))
            pass = std::isnan(f32) == isnan(f16o);
        else
            pass = f16o.bits == f16i.bits && std::isfinite(f32) == isfinite(f16o);

        if (!pass)
        {
            ++errorCount;
            if (errorCount < MAX_ERRORS)
            {
                fprintf(stderr, "FLOAT16 mismatch: expected 0x%04x, got 0x%04x, float value %f\n",
                    f16i.bits, f16o.bits, f32);
            }
        }
    }

    if (errorCount >= MAX_ERRORS)
    {
        fprintf(stderr, "... %u more error(s) ...\n", errorCount - MAX_ERRORS);
    }

    return errorCount == 0;
}

bool test_gpu_float16(nvrhi::IDevice* device)
{
    nvrhi::CommandListHandle commandList = device->createCommandList();

    constexpr size_t count = 65536;
    std::vector<float> f32(count);
    std::vector<float16_t> f16(count);

    std::mt19937 rng(1);
    std::uniform_real_distribution dist(-26.0, 18.0); // These are base-2 logarithms for the input numbers
    std::uniform_int_distribution signDist(0, 1);
    for (size_t i = 0; i < count; ++i)
    {
        double rnd = dist(rng);
        int signRnd = signDist(rng);
        double value = pow(2.0, rnd) * (signRnd ? -1.0 : 1.0);
        f32[i] = float(value);
        f16[i] = Float32ToFloat16(f32[i]);
    }

    auto bufferDesc = nvrhi::BufferDesc()
        .setByteSize(count * sizeof(float))
        .setDebugName("Input Buffer")
        .setCanHaveRawViews(true)
        .enableAutomaticStateTracking(nvrhi::ResourceStates::CopyDest);
    nvrhi::BufferHandle f32buf = device->createBuffer(bufferDesc);

    bufferDesc
        .setByteSize(count * sizeof(float16_t))
        .setDebugName("Output Buffer")
        .setCanHaveUAVs(true);
    nvrhi::BufferHandle f16buf = device->createBuffer(bufferDesc);

    bufferDesc
        .setDebugName("Readback Buffer")
        .setCanHaveUAVs(false)
        .setCpuAccess(nvrhi::CpuAccessMode::Read);
    nvrhi::BufferHandle readbackBuf = device->createBuffer(bufferDesc);

    commandList->open();
    commandList->writeBuffer(f32buf, f32.data(), count * sizeof(float));

    nvrhi::coopvec::ConvertMatrixLayoutDesc convertDesc{};
    convertDesc.numRows = 1;
    convertDesc.numColumns = count;
    convertDesc.src.buffer = f32buf;
    convertDesc.src.layout = nvrhi::coopvec::MatrixLayout::RowMajor;
    convertDesc.src.stride = count * sizeof(float);
    convertDesc.src.size = convertDesc.src.stride;
    convertDesc.src.type = nvrhi::coopvec::DataType::Float32;
    convertDesc.dst.buffer = f16buf;
    convertDesc.dst.layout = nvrhi::coopvec::MatrixLayout::RowMajor;
    convertDesc.dst.stride = count * sizeof(float16_t);
    convertDesc.dst.size = convertDesc.dst.stride;
    convertDesc.dst.type = nvrhi::coopvec::DataType::Float16;
    commandList->convertCoopVecMatrices(&convertDesc, 1);

    commandList->copyBuffer(readbackBuf, 0, f16buf, 0, count * sizeof(float16_t));
    
    commandList->close();
    device->executeCommandList(commandList);
    float16_t const* readbackData = static_cast<float16_t const*>(device->mapBuffer(readbackBuf, nvrhi::CpuAccessMode::Read));

    uint32_t errorCount = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (f16[i] != readbackData[i])
        {
            ++errorCount;
            if (errorCount < MAX_ERRORS)
            {
                fprintf(stderr, "FLOAT16 mismatch [%zu]: CPU produced 0x%04x, GPU produced 0x%04x, float value %f\n",
                    i, f16[i].bits, readbackData[i].bits, f32[i]);
            }
        }
    }

    if (errorCount >= MAX_ERRORS)
    {
        fprintf(stderr, "... %u more error(s) ...\n", errorCount - MAX_ERRORS);
    }

    return errorCount == 0;
}

bool check_known_float8e4m3(float in, uint8_t out)
{
    float8e4m3_t f8o = Float32ToFloat8E4M3(in);
    float f32o = Float8E4M3ToFloat32(f8o);

    bool pass = f8o.bits == out;
    if (std::isinf(in) || std::isnan(in))
        pass = pass && isnan(f8o) && std::isnan(f32o);
    
    if (!pass)
    {
        fprintf(stderr, "Known E4M3 mismatch: expected 0x%02x, got 0x%02x, float value %f\n",
            out, f8o.bits, in);
    }

    return pass;
}

bool test_known_float8e4m3()
{
    // See https://asawicki.info/articles/fp8_tables.php
    
    using f32lim = std::numeric_limits<float>;
    using f8lim = std::numeric_limits<float8e4m3_t>;

    bool pass = true;
    pass &= check_known_float8e4m3(0.f, 0);
    pass &= check_known_float8e4m3(0.001953f, f8lim::denorm_min().bits); // Smallest denormal
    pass &= check_known_float8e4m3(0.01562f, f8lim::min().bits); // Smallest normal
    pass &= check_known_float8e4m3(1.0f, 0x38);
    pass &= check_known_float8e4m3(448.f, f8lim::max().bits); // Largest normal
    pass &= check_known_float8e4m3(-6.5f, 0xcd); // Some negative number
    pass &= check_known_float8e4m3(600.f, f8lim::quiet_NaN().bits); // Overflow into NaN
    pass &= check_known_float8e4m3(f32lim::quiet_NaN(), f8lim::quiet_NaN().bits);
    return pass;
}

bool test_float8e4m3()
{
    float8e4m3_t f8i;
    float8e4m3_t f8o;
    float f32;
    uint32_t errorCount = 0;
    for (int i = 0; i < 256; ++i)
    {
        f8i.bits = i;
        f32 = Float8E4M3ToFloat32(f8i);
        f8o = Float32ToFloat8E4M3(f32);

        bool pass;
        CHECK(!isinf(f8i)); // E4M3 doesn't support INF
        if (isnan(f8i))
            pass = std::isnan(f32) == isnan(f8o);
        else
            pass = f8o.bits == f8i.bits && std::isfinite(f32) == isfinite(f8o);

        if (!pass)
        {
            ++errorCount;
            if (errorCount < MAX_ERRORS)
            {
                fprintf(stderr, "E4M3 mismatch: expected 0x%02x, got 0x%02x, float value %f\n",
                    f8i.bits, f8o.bits, f32);
            }
        }
    }

    if (errorCount >= MAX_ERRORS)
    {
        fprintf(stderr, "... %u more error(s) ...\n", errorCount - MAX_ERRORS);
    }

    return errorCount == 0;
}

bool check_known_float8e5m2(float in, uint8_t out)
{
    float8e5m2_t f8o = Float32ToFloat8E5M2(in);
    float f32o = Float8E5M2ToFloat32(f8o);

    bool pass = f8o.bits == out;
    if (std::isinf(in))
        pass = pass && isinf(f8o) && std::isinf(f32o);
    else if (std::isnan(in))
        pass = pass && isnan(f8o) && std::isnan(f32o);
    
    if (!pass)
    {
        fprintf(stderr, "Known E5M2 mismatch: expected 0x%02x, got 0x%02x, float value %f\n",
            out, f8o.bits, in);
    }

    return pass;
}

bool test_known_float8e5m2()
{
    // See https://asawicki.info/articles/fp8_tables.php

    using f32lim = std::numeric_limits<float>;
    using f8lim = std::numeric_limits<float8e5m2_t>;

    bool pass = true;
    pass &= check_known_float8e5m2(0.f, 0);
    pass &= check_known_float8e5m2(0.0000153f, f8lim::denorm_min().bits); // Smallest denormal
    pass &= check_known_float8e5m2(0.000061f, f8lim::min().bits); // Smallest normal
    pass &= check_known_float8e5m2(1.0f, 0x3c);
    pass &= check_known_float8e5m2(57344.f, f8lim::max().bits); // Largest normal
    pass &= check_known_float8e5m2(-0.375f, 0xb6); // Some negative number
    pass &= check_known_float8e5m2(65504.f, f8lim::infinity().bits); // Overflow into Inf
    pass &= check_known_float8e5m2(f32lim::infinity(), f8lim::infinity().bits);
    pass &= check_known_float8e5m2(f32lim::quiet_NaN(), f8lim::quiet_NaN().bits);
    return pass;
}

bool test_float8e5m2()
{
    float8e5m2_t f8i;
    float8e5m2_t f8o;
    float f32;
    uint32_t errorCount = 0;
    for (int i = 0; i < 256; ++i)
    {
        f8i.bits = i;
        f32 = Float8E5M2ToFloat32(f8i);
        f8o = Float32ToFloat8E5M2(f32);

        bool pass;
        if (isinf(f8i))
            pass = std::isinf(f32) == isinf(f8o) && signbit(f8i) == signbit(f8o);
        else if (isnan(f8i))
            pass = std::isnan(f32) == isnan(f8o);
        else
            pass = f8o.bits == f8i.bits && std::isfinite(f32) == isfinite(f8o);

        if (!pass)
        {
            ++errorCount;
            if (errorCount < MAX_ERRORS)
            {
                fprintf(stderr, "E5M2 mismatch: expected 0x%02x, got 0x%02x, float value %f\n",
                    f8i.bits, f8o.bits, f32);
            }
        }
    }

    if (errorCount >= MAX_ERRORS)
    {
        fprintf(stderr, "... %u more error(s) ...\n", errorCount - MAX_ERRORS);
    }

    return errorCount == 0;
}

bool test_gpu_float8(nvrhi::IDevice* device, bool e5m2)
{
    nvrhi::CommandListHandle commandList = device->createCommandList();

    constexpr size_t count = 65536;
    std::vector<float> f32(count);
    std::vector<uint8_t> f8(count);

    std::mt19937 rng(1);
    // These are base-2 logarithms for the input numbers
    double minLog = e5m2 ? -18.0 : -11.0;
    double maxLog = e5m2 ? 17.0 : 10.0;
    std::uniform_real_distribution dist(minLog, maxLog);
    std::uniform_int_distribution signDist(0, 1);
    for (size_t i = 0; i < count; ++i)
    {
        double rnd = dist(rng);
        int signRnd = signDist(rng);

        double value = pow(2.0, rnd) * (signRnd ? -1.0 : 1.0);
        f32[i] = float(value);
        
        if (e5m2)
            f8[i] = Float32ToFloat8E5M2(f32[i]).bits;
        else
            f8[i] = Float32ToFloat8E4M3(f32[i]).bits;
    }

    auto bufferDesc = nvrhi::BufferDesc()
        .setByteSize(count * sizeof(float))
        .setDebugName("Input Buffer")
        .setCanHaveRawViews(true)
        .enableAutomaticStateTracking(nvrhi::ResourceStates::CopyDest);
    nvrhi::BufferHandle f32buf = device->createBuffer(bufferDesc);

    bufferDesc
        .setByteSize(count * sizeof(uint8_t))
        .setDebugName("Output Buffer")
        .setCanHaveUAVs(true);
    nvrhi::BufferHandle f8buf = device->createBuffer(bufferDesc);

    bufferDesc
        .setDebugName("Readback Buffer")
        .setCanHaveUAVs(false)
        .setCpuAccess(nvrhi::CpuAccessMode::Read);
    nvrhi::BufferHandle readbackBuf = device->createBuffer(bufferDesc);

    commandList->open();
    commandList->writeBuffer(f32buf, f32.data(), count * sizeof(float));

    nvrhi::coopvec::ConvertMatrixLayoutDesc convertDesc{};
    convertDesc.numRows = 1;
    convertDesc.numColumns = count;
    convertDesc.src.buffer = f32buf;
    convertDesc.src.layout = nvrhi::coopvec::MatrixLayout::RowMajor;
    convertDesc.src.stride = count * sizeof(float);
    convertDesc.src.size = convertDesc.src.stride;
    convertDesc.src.type = nvrhi::coopvec::DataType::Float32;
    convertDesc.dst.buffer = f8buf;
    convertDesc.dst.layout = nvrhi::coopvec::MatrixLayout::RowMajor;
    convertDesc.dst.stride = count * sizeof(uint8_t);
    convertDesc.dst.size = convertDesc.dst.stride;
    convertDesc.dst.type = e5m2
        ? nvrhi::coopvec::DataType::FloatE5M2
        : nvrhi::coopvec::DataType::FloatE4M3;
    commandList->convertCoopVecMatrices(&convertDesc, 1);

    commandList->copyBuffer(readbackBuf, 0, f8buf, 0, count * sizeof(uint8_t));
    
    commandList->close();
    device->executeCommandList(commandList);
    uint8_t const* readbackData = static_cast<uint8_t const*>(device->mapBuffer(readbackBuf, nvrhi::CpuAccessMode::Read));

    uint32_t errorCount = 0;
    for (size_t i = 0; i < count; ++i)
    {
        uint8_t cpui8 = f8[i];
        uint8_t gpui8 = readbackData[i];

        // Allow 1 ULP of difference. Rounding with vkCmdConvertCooperativeVectorMatrixNV can be inexact for inputs
        // that are very close to a value in the middle between two representable FP8 numbers but is slightly off.
        // See nvbug 5775011
        bool pass = abs(int(cpui8) - int(gpui8)) <= 1;
        if (!e5m2 && !pass)
        {
            // Special case: treat +NaN == -NaN 
            if (cpui8 == 0xff && gpui8 == 0x7f)
                pass = true;
        }

        if (!pass)
        {
            ++errorCount;
            if (errorCount < MAX_ERRORS)
            {
                float cpuResult = e5m2 ? Float8E5M2ToFloat32({ cpui8 }) : Float8E4M3ToFloat32({ cpui8 });
                float gpuResult = e5m2 ? Float8E5M2ToFloat32({ gpui8 }) : Float8E4M3ToFloat32({ gpui8 });

                fprintf(stderr, "%s mismatch [%zu]: CPU produced 0x%02x (%f), GPU produced 0x%02x (%f), input value %f\n",
                    e5m2 ? "E5M2" : "E4M3", i, cpui8, cpuResult, gpui8, gpuResult, f32[i]);
            }
        }
    }

    if (errorCount >= MAX_ERRORS)
    {
        fprintf(stderr, "... %u more error(s) ...\n", errorCount - MAX_ERRORS);
    }

    return errorCount == 0;
}

std::unique_ptr<donut::app::DeviceManager> InitializeGraphicsDevice(nvrhi::GraphicsAPI graphicsApi)
{
    using namespace donut::app;

    std::unique_ptr<DeviceManager> deviceManager = std::unique_ptr<DeviceManager>(
        DeviceManager::Create(graphicsApi));

    if (!deviceManager)
    {
        fprintf(stderr, "Failed to create a %s DeviceManager, skipping GPU tests.\n",
            nvrhi::utils::GraphicsAPIToString(graphicsApi));
        return nullptr;
    }
    
#if DONUT_WITH_DX12
    if (graphicsApi == nvrhi::GraphicsAPI::D3D12)
    {
        UUID Features[] = { D3D12ExperimentalShaderModels, D3D12CooperativeVectorExperiment };
        HRESULT hr = D3D12EnableExperimentalFeatures(_countof(Features), Features, nullptr, nullptr);
        if (FAILED(hr))
        {
            fprintf(stderr, "Failed to enable D3D12 experimental shader models, skipping GPU tests.\n");
            return nullptr;
        }
    }
#endif

    DeviceCreationParameters deviceParams;
    deviceParams.requiredVulkanDeviceExtensions.push_back(VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME);
    if (!deviceManager->CreateHeadlessDevice(deviceParams))
    {
        fprintf(stderr, "Failed to create a %s device, skipping GPU tests.\n",
            nvrhi::utils::GraphicsAPIToString(graphicsApi));
        return nullptr;
    }

    if (!deviceManager->GetDevice()->queryFeatureSupport(nvrhi::Feature::CooperativeVectorInferencing))
    {
        fprintf(stderr, "%s device \"%s\" doesn't support Cooperative Vectors, skipping GPU tests.\n",
            nvrhi::utils::GraphicsAPIToString(graphicsApi), deviceManager->GetRendererString());
        return nullptr;
    }

    return std::move(deviceManager);
}

bool ReportTestResult(char const* name, bool pass)
{
    printf("%s: %s\n", name, pass ? "PASS" : "FAIL");
    return pass;
}

int main(int argc, char** argv)
{
    donut::log::ConsoleApplicationMode();
    donut::log::SetMinSeverity(donut::log::Severity::Warning);

    nvrhi::GraphicsAPI graphicsApi = donut::app::GetGraphicsAPIFromCommandLine(argc, argv);
    std::unique_ptr<donut::app::DeviceManager> deviceManager = InitializeGraphicsDevice(graphicsApi);

    bool f16c = donut::math::IsF16CSupported();

    bool pass = true;

    donut::math::EnableF16C(false);
    pass &= ReportTestResult("FP32 -> Known FP16", test_known_float16());
    if (f16c)
    {
        donut::math::EnableF16C(true);
        pass &= ReportTestResult("FP32 -> Known FP16 (HW)", test_known_float16());
    }
    pass &= ReportTestResult("FP32 -> Known E4M3", test_known_float8e4m3());
    pass &= ReportTestResult("FP32 -> Known E5M2", test_known_float8e5m2());
    
    donut::math::EnableF16C(false);
    pass &= ReportTestResult("All FP16 -> FP32 -> FP16", test_float16());
    if (f16c)
    {
        donut::math::EnableF16C(true);
        pass &= ReportTestResult("All FP16 -> FP32 -> FP16 (HW)", test_float16());
    }
    pass &= ReportTestResult("All E4M3 -> FP32 -> E4M3", test_float8e4m3());
    pass &= ReportTestResult("All E5M2 -> FP32 -> E5M2", test_float8e5m2());

    donut::math::EnableF16C(false);
    pass &= ReportTestResult("FP16 vectors", test_float16_vectors());
    if (f16c)
    {
        donut::math::EnableF16C(true);
        pass &= ReportTestResult("FP16 vectors (HW)", test_float16_vectors());
    }

    if (deviceManager && deviceManager->GetDevice())
    {
        std::string graphicsApiString = nvrhi::utils::GraphicsAPIToString(graphicsApi);

        std::string testName = "Random FP32 -> FP16 vs. " + graphicsApiString;
        donut::math::EnableF16C(false);
        pass &= ReportTestResult(testName.c_str(), test_gpu_float16(deviceManager->GetDevice()));

        testName = "Random FP32 -> E4M3 vs. " + graphicsApiString;
        pass &= ReportTestResult(testName.c_str(), test_gpu_float8(deviceManager->GetDevice(), false));

        testName = "Random FP32 -> E5M2 vs. " + graphicsApiString;
        pass &= ReportTestResult(testName.c_str(), test_gpu_float8(deviceManager->GetDevice(), true));
    }
	
    return pass ? 0 : 1;
}
