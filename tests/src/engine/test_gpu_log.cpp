// Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
// SPDX-License-Identifier: MIT

#include <donut/tests/GpuTestLog.h>
#include <donut/tests/utils.h>

int main()
{
    using donut::log::Severity;
    using donut::tests::LogErrorCounter;
    try
    {
        const char* knownWarning = "[Vulkan: location=0xc81ad50e code=0, layerPrefix='Validation'] "
            "vkCreateGraphicsPipelines(): pCreateInfos[0].pVertexInputState "
            "Vertex attribute at location 1 not consumed by vertex shader.";
        CHECK(!LogErrorCounter::IsError(Severity::Warning, knownWarning));
        CHECK(LogErrorCounter::IsError(Severity::Error, knownWarning));
        // Native errors need not contain a severity label in their message text.
        CHECK(LogErrorCounter::IsError(Severity::Warning,
            "[Vulkan: location=0x123 code=0, layerPrefix='Validation'] vkCmdDraw(): descriptor set is not bound."));
        CHECK(LogErrorCounter::IsError(Severity::Warning,
            "[Vulkan: location=0x123 code=0, layerPrefix='Validation'] Another native diagnostic."));
        CHECK(LogErrorCounter::IsError(Severity::Warning, "[Vulkan: truncated message"));
        // The debug-utils callback preserves native severity, so ordinary warnings
        // from it need no conservative reclassification.
        CHECK(!LogErrorCounter::IsError(Severity::Warning, "[Vulkan debug-utils: ID] A warning."));
        CHECK(LogErrorCounter::IsError(Severity::Error, "[Vulkan debug-utils: ID] An error."));
        CHECK(!LogErrorCounter::IsError(Severity::Warning, "FP16 input falls back to FP32."));
        CHECK(LogErrorCounter::IsError(Severity::Error, "NVRHI validation error."));
        std::puts("GPU validation log classification: PASS");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
