// Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <donut/core/log.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace donut::tests
{
    class LogErrorCounter
    {
        log::Callback previous = log::GetCallback();
        bool reportNativeMessages;

    public:
        std::atomic<uint32_t> errors{ 0 };
        std::atomic<uint32_t> nativeWarnings{ 0 };

        static bool IsError(log::Severity severity, const char* message)
        {
            if (severity >= log::Severity::Error)
                return true;
            if (std::strncmp(message, "[Vulkan:", 8) != 0)
                return false;

            // DeviceManager's legacy debug-report callback logs errors as warnings. Fail on
            // unclassified messages, allowing only this known harmless diagnostic.
            // Do not infer severity from an "Error" substring: current layers omit it.
            const char* payload = std::strstr(message, "] ");
            return !payload || std::strcmp(payload + 2,
                "vkCreateGraphicsPipelines(): pCreateInfos[0].pVertexInputState "
                "Vertex attribute at location 1 not consumed by vertex shader.") != 0;
        }

        explicit LogErrorCounter(bool reportNativeMessages)
            : reportNativeMessages(reportNativeMessages)
        {
            log::SetCallback([this](log::Severity severity, const char* message)
            {
                if (IsError(severity, message))
                    ++errors;
                else if (std::strncmp(message, "[Vulkan:", 8) == 0
                    || std::strncmp(message, "[Vulkan debug-utils:", 20) == 0)
                    ++nativeWarnings;
                previous(severity, message);
            });
        }

        ~LogErrorCounter() { log::SetCallback(previous); }
        LogErrorCounter(const LogErrorCounter&) = delete;
        LogErrorCounter& operator=(const LogErrorCounter&) = delete;

        void Report() const
        {
            if (reportNativeMessages)
                std::printf("Validation log: %u errors or unclassified native messages, %u known native warnings (all messages reported)\n",
                    errors.load(), nativeWarnings.load());
        }
    };
}
