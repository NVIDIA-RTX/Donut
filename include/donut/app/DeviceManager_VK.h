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
#pragma once

#include <string>
#include <queue>
#include <unordered_set>

#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>

#include <nvrhi/vulkan.h>
#include <nvrhi/validation.h>

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#include <vulkan/vulkan.hpp>

class DeviceManager_VK : public donut::app::DeviceManager
{
public:
    [[nodiscard]] nvrhi::IDevice* GetDevice() const override
    {
        if (m_ValidationLayer)
            return m_ValidationLayer;

        return m_NvrhiDevice;
    }
    
    [[nodiscard]] nvrhi::GraphicsAPI GetGraphicsAPI() const override
    {
        return nvrhi::GraphicsAPI::VULKAN;
    }

    bool EnumerateAdapters(std::vector<donut::app::AdapterInfo>& outAdapters) override;
    const donut::app::DeviceCreationParameters& GetDeviceParams() const { return m_DeviceParams; };

protected:
    bool CreateInstanceInternal() override;
    bool CreateDevice() override;
    bool CreateSwapChain() override;
    void DestroyDeviceAndSwapChain() override;

    void ResizeSwapChain() override
    {
        if (m_VulkanDevice)
        {
            destroySwapChain();
            createSwapChain();
        }
    }

    nvrhi::ITexture* GetCurrentBackBuffer() override
    {
        return m_SwapChainImages[m_SwapChainIndex].rhiHandle;
    }
    nvrhi::ITexture* GetBackBuffer(uint32_t index) override
    {
        if (index < m_SwapChainImages.size())
            return m_SwapChainImages[index].rhiHandle;
        return nullptr;
    }
    uint32_t GetCurrentBackBufferIndex() override
    {
        return m_SwapChainIndex;
    }
    uint32_t GetBackBufferCount() override
    {
        return uint32_t(m_SwapChainImages.size());
    }

    bool BeginFrame() override;
    bool Present() override;

    const char *GetRendererString() const override
    {
        return m_RendererString.c_str();
    }

    bool IsVulkanInstanceExtensionEnabled(const char* extensionName) const override
    {
        return enabledExtensions.instance.find(extensionName) != enabledExtensions.instance.end();
    }

    bool IsVulkanDeviceExtensionEnabled(const char* extensionName) const override
    {
        return enabledExtensions.device.find(extensionName) != enabledExtensions.device.end();
    }
    
    bool IsVulkanLayerEnabled(const char* layerName) const override
    {
        return enabledExtensions.layers.find(layerName) != enabledExtensions.layers.end();
    }

    void GetEnabledVulkanInstanceExtensions(std::vector<std::string>& extensions) const override
    {
        for (const auto& ext : enabledExtensions.instance)
            extensions.push_back(ext);
    }

    void GetEnabledVulkanDeviceExtensions(std::vector<std::string>& extensions) const override
    {
        for (const auto& ext : enabledExtensions.device)
            extensions.push_back(ext);
    }

    void GetEnabledVulkanLayers(std::vector<std::string>& layers) const override
    {
        for (const auto& ext : enabledExtensions.layers)
            layers.push_back(ext);
    }

    bool createInstance();
    bool createWindowSurface();
    void installDebugCallback();
    bool pickPhysicalDevice();
    bool findQueueFamilies(vk::PhysicalDevice physicalDevice);
    bool createDevice();
    bool createSwapChain();
    void destroySwapChain();

    struct VulkanExtensionSet
    {
        std::unordered_set<std::string> instance;
        std::unordered_set<std::string> layers;
        std::unordered_set<std::string> device;
    };

    // minimal set of required extensions
    VulkanExtensionSet enabledExtensions = {
        // instance
        {
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME
        },
        // layers
        { },
        // device
        { },
    };

    // optional extensions
    VulkanExtensionSet optionalExtensions = {
        // instance
        { 
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
            VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME,
        },
        // layers
        { },
        // device
        { 
            VK_EXT_DEBUG_MARKER_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
            VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME,
            VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
            VK_EXT_MESH_SHADER_EXTENSION_NAME,
            VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
#if DONUT_WITH_AFTERMATH
            VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME,
            VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME,
#endif
        },
    };

    std::unordered_set<std::string> m_RayTracingExtensions = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME
    };

    std::string m_RendererString;

    vk::Instance m_VulkanInstance;
    vk::DebugReportCallbackEXT m_DebugReportCallback;

    vk::PhysicalDevice m_VulkanPhysicalDevice;
    int m_GraphicsQueueFamily = -1;
    int m_ComputeQueueFamily = -1;
    int m_TransferQueueFamily = -1;
    int m_PresentQueueFamily = -1;

    vk::Device m_VulkanDevice;
    vk::Queue m_GraphicsQueue;
    vk::Queue m_ComputeQueue;
    vk::Queue m_TransferQueue;
    vk::Queue m_PresentQueue;
    
    vk::SurfaceKHR m_WindowSurface;

    vk::SurfaceFormatKHR m_SwapChainFormat;
    vk::SwapchainKHR m_SwapChain;
    bool m_SwapChainMutableFormatSupported = false;

    struct SwapChainImage
    {
        vk::Image image;
        nvrhi::TextureHandle rhiHandle;
    };

    std::vector<SwapChainImage> m_SwapChainImages;
    uint32_t m_SwapChainIndex = uint32_t(-1);

    nvrhi::vulkan::DeviceHandle m_NvrhiDevice;
    nvrhi::DeviceHandle m_ValidationLayer;

    std::vector<vk::Semaphore> m_AcquireSemaphores;
    std::vector<vk::Semaphore> m_PresentSemaphores;
    uint32_t m_AcquireSemaphoreIndex = 0;

    std::queue<nvrhi::EventQueryHandle> m_FramesInFlight;
    std::vector<nvrhi::EventQueryHandle> m_QueryPool;

    bool m_BufferDeviceAddressSupported = false;

#if VK_HEADER_VERSION >= 301
    typedef vk::detail::DynamicLoader VulkanDynamicLoader;
#else
    typedef vk::DynamicLoader VulkanDynamicLoader;
#endif

    std::unique_ptr<VulkanDynamicLoader> m_dynamicLoader;
};