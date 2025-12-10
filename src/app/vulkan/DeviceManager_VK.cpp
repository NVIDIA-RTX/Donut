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
#include <queue>
#include <unordered_set>
#include <memory>
#include <sstream>

#include <donut/app/DeviceManager.h>
#include <donut/app/DeviceManager_VK.h>

#include <nvrhi/vulkan.h>
#include <nvrhi/validation.h>

#if DONUT_WITH_STREAMLINE
#include <StreamlineIntegration.h>
#endif

// Define the Vulkan dynamic dispatcher - this needs to occur in exactly one cpp file in the program.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

using namespace donut;
using namespace donut::app;

static constexpr uint32_t kComputeQueueIndex = 0;
static constexpr uint32_t kGraphicsQueueIndex = 0;
static constexpr uint32_t kPresentQueueIndex = 0;
static constexpr uint32_t kTransferQueueIndex = 0;

static std::vector<const char *> stringSetToVector(const std::unordered_set<std::string>& set)
{
    std::vector<const char *> ret;
    for(const auto& s : set)
    {
        ret.push_back(s.c_str());
    }

    return ret;
}

template <typename T>
static std::vector<T> setToVector(const std::unordered_set<T>& set)
{
    std::vector<T> ret;
    for(const auto& s : set)
    {
        ret.push_back(s);
    }

    return ret;
}

bool DeviceManager_VK::createInstance()
{
    if (!m_DeviceParams.headlessDevice)
    {
        if (!glfwVulkanSupported())
        {
            log::error("GLFW reports that Vulkan is not supported. Perhaps missing a call to glfwInit()?");
            return false;
        }

        // add any extensions required by GLFW
        uint32_t glfwExtCount;
        const char **glfwExt = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        assert(glfwExt);

        for(uint32_t i = 0; i < glfwExtCount; i++)
        {
            enabledExtensions.instance.insert(std::string(glfwExt[i]));
        }
    }

    // add instance extensions requested by the user
    for (const std::string& name : m_DeviceParams.requiredVulkanInstanceExtensions)
    {
        enabledExtensions.instance.insert(name);
    }
    for (const std::string& name : m_DeviceParams.optionalVulkanInstanceExtensions)
    {
        optionalExtensions.instance.insert(name);
    }

    // add layers requested by the user
    for (const std::string& name : m_DeviceParams.requiredVulkanLayers)
    {
        enabledExtensions.layers.insert(name);
    }
    for (const std::string& name : m_DeviceParams.optionalVulkanLayers)
    {
        optionalExtensions.layers.insert(name);
    }

    std::unordered_set<std::string> requiredExtensions = enabledExtensions.instance;

    // figure out which optional extensions are supported
    for(const auto& instanceExt : vk::enumerateInstanceExtensionProperties())
    {
        const std::string name = instanceExt.extensionName;
        if (optionalExtensions.instance.find(name) != optionalExtensions.instance.end())
        {
            enabledExtensions.instance.insert(name);
        }

        requiredExtensions.erase(name);
    }

    if (!requiredExtensions.empty())
    {
        std::stringstream ss;
        ss << "Cannot create a Vulkan instance because the following required extension(s) are not supported:";
        for (const auto& ext : requiredExtensions)
            ss << std::endl << "  - " << ext;

        log::error("%s", ss.str().c_str());
        return false;
    }

    log::message(m_DeviceParams.infoLogSeverity, "Enabled Vulkan instance extensions:");
    for (const auto& ext : enabledExtensions.instance)
    {
        log::message(m_DeviceParams.infoLogSeverity, "    %s", ext.c_str());
    }

    std::unordered_set<std::string> requiredLayers = enabledExtensions.layers;

    for(const auto& layer : vk::enumerateInstanceLayerProperties())
    {
        const std::string name = layer.layerName;
        if (optionalExtensions.layers.find(name) != optionalExtensions.layers.end())
        {
            enabledExtensions.layers.insert(name);
        }

        requiredLayers.erase(name);
    }

    if (!requiredLayers.empty())
    {
        std::stringstream ss;
        ss << "Cannot create a Vulkan instance because the following required layer(s) are not supported:";
        for (const auto& ext : requiredLayers)
            ss << std::endl << "  - " << ext;

        log::error("%s", ss.str().c_str());
        return false;
    }
    
    log::message(m_DeviceParams.infoLogSeverity, "Enabled Vulkan layers:");
    for (const auto& layer : enabledExtensions.layers)
    {
        log::message(m_DeviceParams.infoLogSeverity, "    %s", layer.c_str());
    }

    auto instanceExtVec = stringSetToVector(enabledExtensions.instance);
    auto layerVec = stringSetToVector(enabledExtensions.layers);
    
    auto applicationInfo = vk::ApplicationInfo();

    // Query the Vulkan API version supported on the system to make sure we use at least 1.3 when that's present.
    vk::Result res = vk::enumerateInstanceVersion(&applicationInfo.apiVersion);

    if (res != vk::Result::eSuccess)
    {
        log::error("Call to vkEnumerateInstanceVersion failed, error code = %s", nvrhi::vulkan::resultToString(VkResult(res)));
        return false;
    }

    const uint32_t minimumVulkanVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);

    // Check if the Vulkan API version is sufficient.
    if (applicationInfo.apiVersion < minimumVulkanVersion)
    {
        log::error("The Vulkan API version supported on the system (%d.%d.%d) is too low, at least %d.%d.%d is required.",
            VK_API_VERSION_MAJOR(applicationInfo.apiVersion), VK_API_VERSION_MINOR(applicationInfo.apiVersion), VK_API_VERSION_PATCH(applicationInfo.apiVersion),
            VK_API_VERSION_MAJOR(minimumVulkanVersion), VK_API_VERSION_MINOR(minimumVulkanVersion), VK_API_VERSION_PATCH(minimumVulkanVersion));
        return false;
    }

    // Spec says: A non-zero variant indicates the API is a variant of the Vulkan API and applications will typically need to be modified to run against it.
    if (VK_API_VERSION_VARIANT(applicationInfo.apiVersion) != 0)
    {
        log::error("The Vulkan API supported on the system uses an unexpected variant: %d.", VK_API_VERSION_VARIANT(applicationInfo.apiVersion));
        return false;
    }

    // Create the vulkan instance
    vk::InstanceCreateInfo info = vk::InstanceCreateInfo()
        .setEnabledLayerCount(uint32_t(layerVec.size()))
        .setPpEnabledLayerNames(layerVec.data())
        .setEnabledExtensionCount(uint32_t(instanceExtVec.size()))
        .setPpEnabledExtensionNames(instanceExtVec.data())
        .setPApplicationInfo(&applicationInfo);

    res = vk::createInstance(&info, nullptr, &m_VulkanInstance);
    if (res != vk::Result::eSuccess)
    {
        log::error("Failed to create a Vulkan instance, error code = %s", nvrhi::vulkan::resultToString(VkResult(res)));
        return false;
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_VulkanInstance);

    return true;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL vulkanDebugCallback(
    vk::DebugReportFlagsEXT flags,
    vk::DebugReportObjectTypeEXT objType,
    uint64_t obj,
    size_t location,
    int32_t code,
    const char* layerPrefix,
    const char* msg,
    void* userData)
{
    const DeviceManager_VK* manager = (const DeviceManager_VK*)userData;

    if (manager)
    {
        const auto& ignored = manager->GetDeviceParams().ignoredVulkanValidationMessageLocations;
        const auto found = std::find(ignored.begin(), ignored.end(), location);
        if (found != ignored.end())
            return VK_FALSE;
    }

    donut::log::warning("[Vulkan: location=0x%zx code=%d, layerPrefix='%s'] %s", location, code, layerPrefix, msg);

    return VK_FALSE;
}

void DeviceManager_VK::installDebugCallback()
{
    auto info = vk::DebugReportCallbackCreateInfoEXT()
                    .setFlags(vk::DebugReportFlagBitsEXT::eError |
                              vk::DebugReportFlagBitsEXT::eWarning |
                            //   vk::DebugReportFlagBitsEXT::eInformation |
                              vk::DebugReportFlagBitsEXT::ePerformanceWarning)
                    .setPfnCallback(vulkanDebugCallback)
                    .setPUserData(this);

    vk::Result res = m_VulkanInstance.createDebugReportCallbackEXT(&info, nullptr, &m_DebugReportCallback);
    assert(res == vk::Result::eSuccess);
    (void)res;
}

bool DeviceManager_VK::pickPhysicalDevice()
{
    VkFormat requestedFormat = nvrhi::vulkan::convertFormat(m_DeviceParams.swapChainFormat);
    vk::Extent2D requestedExtent(m_DeviceParams.backBufferWidth, m_DeviceParams.backBufferHeight);

    auto devices = m_VulkanInstance.enumeratePhysicalDevices();

    int adapterIndex = m_DeviceParams.adapterIndex;

    int firstDevice = 0;
    int lastDevice = int(devices.size()) - 1;
    if (adapterIndex >= 0)
    {
        if (adapterIndex > lastDevice)
        {
            log::error("The specified Vulkan physical device %d does not exist.", adapterIndex);
            return false;
        }
        firstDevice = adapterIndex;
        lastDevice = adapterIndex;
    }

    // Start building an error message in case we cannot find a device.
    std::stringstream errorStream;
    errorStream << "Cannot find a Vulkan device that supports all the required extensions and properties.";

    // build a list of GPUs
    std::vector<vk::PhysicalDevice> discreteGPUs;
    std::vector<vk::PhysicalDevice> otherGPUs;
    for (int deviceIndex = firstDevice; deviceIndex <= lastDevice; ++deviceIndex)
    {
        vk::PhysicalDevice const& dev = devices[deviceIndex];
        vk::PhysicalDeviceProperties prop = dev.getProperties();

        errorStream << std::endl << prop.deviceName.data() << ":";

        // check that all required device extensions are present
        std::unordered_set<std::string> requiredExtensions = enabledExtensions.device;
        auto deviceExtensions = dev.enumerateDeviceExtensionProperties();
        for(const auto& ext : deviceExtensions)
        {
            requiredExtensions.erase(std::string(ext.extensionName.data()));
        }

        bool deviceIsGood = true;

        if (!requiredExtensions.empty())
        {
            // device is missing one or more required extensions
            for (const auto& ext : requiredExtensions)
            {
                errorStream << std::endl << "  - missing " << ext;
            }
            deviceIsGood = false;
        }

        vk::PhysicalDeviceFeatures2 deviceFeatures2{};
        vk::PhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{};
        vk::PhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV linearSweptSpheresFeatures{};
        deviceFeatures2.pNext = &dynamicRenderingFeatures;
        dynamicRenderingFeatures.pNext = &linearSweptSpheresFeatures;

        dev.getFeatures2(&deviceFeatures2);
        if (!deviceFeatures2.features.samplerAnisotropy)
        {
            // device is a toaster oven
            errorStream << std::endl << "  - does not support samplerAnisotropy";
            deviceIsGood = false;
        }
        if (!deviceFeatures2.features.textureCompressionBC)
        {
            errorStream << std::endl << "  - does not support textureCompressionBC";
            deviceIsGood = false;
        }
        if (!dynamicRenderingFeatures.dynamicRendering)
        {
            errorStream << std::endl << "  - does not support dynamicRendering";
            deviceIsGood = false;
        }
        if (!linearSweptSpheresFeatures.spheres)
        {
            errorStream << std::endl << "  - does not support sphere primitives";
            deviceIsGood = false;
        }
        if (!linearSweptSpheresFeatures.linearSweptSpheres)
        {
            errorStream << std::endl << "  - does not support Linear-Swept-Sphere(LSS) primitives";
            deviceIsGood = false;
        }

        if (!findQueueFamilies(dev))
        {
            // device doesn't have all the queue families we need
            errorStream << std::endl << "  - does not support the necessary queue types";
            deviceIsGood = false;
        }

        if (deviceIsGood && m_WindowSurface)
        {
            bool surfaceSupported = dev.getSurfaceSupportKHR(m_PresentQueueFamily, m_WindowSurface);
            if (!surfaceSupported)
            {
                errorStream << std::endl << "  - does not support the window surface";
                deviceIsGood = false;
            }
            else
            {
                // check that this device supports our intended swap chain creation parameters
                auto surfaceCaps = dev.getSurfaceCapabilitiesKHR(m_WindowSurface);
                auto surfaceFmts = dev.getSurfaceFormatsKHR(m_WindowSurface);

                if (surfaceCaps.minImageCount > m_DeviceParams.swapChainBufferCount ||
                    (surfaceCaps.maxImageCount < m_DeviceParams.swapChainBufferCount && surfaceCaps.maxImageCount > 0))
                {
                    errorStream << std::endl << "  - cannot support the requested swap chain image count:";
                    errorStream << " requested " << m_DeviceParams.swapChainBufferCount << ", available " << surfaceCaps.minImageCount << " - " << surfaceCaps.maxImageCount;
                    deviceIsGood = false;
                }

                if (surfaceCaps.minImageExtent.width > requestedExtent.width ||
                    surfaceCaps.minImageExtent.height > requestedExtent.height ||
                    surfaceCaps.maxImageExtent.width < requestedExtent.width ||
                    surfaceCaps.maxImageExtent.height < requestedExtent.height)
                {
                    errorStream << std::endl << "  - cannot support the requested swap chain size:";
                    errorStream << " requested " << requestedExtent.width << "x" << requestedExtent.height << ", ";
                    errorStream << " available " << surfaceCaps.minImageExtent.width << "x" << surfaceCaps.minImageExtent.height;
                    errorStream << " - " << surfaceCaps.maxImageExtent.width << "x" << surfaceCaps.maxImageExtent.height;
                    deviceIsGood = false;
                }

                bool surfaceFormatPresent = false;
                for (const vk::SurfaceFormatKHR& surfaceFmt : surfaceFmts)
                {
                    if (surfaceFmt.format == vk::Format(requestedFormat))
                    {
                        surfaceFormatPresent = true;
                        break;
                    }
                }

                if (!surfaceFormatPresent)
                {
                    // can't create a swap chain using the format requested
                    errorStream << std::endl << "  - does not support the requested swap chain format";
                    deviceIsGood = false;
                }

                // check that we can present from the graphics queue
                uint32_t canPresent = dev.getSurfaceSupportKHR(m_GraphicsQueueFamily, m_WindowSurface);
                if (!canPresent)
                {
                    errorStream << std::endl << "  - cannot present";
                    deviceIsGood = false;
                }
            }
        }

        if (!deviceIsGood)
            continue;

        if (prop.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
        {
            discreteGPUs.push_back(dev);
        }
        else
        {
            otherGPUs.push_back(dev);
        }
    }

    // pick the first discrete GPU if it exists, otherwise the first integrated GPU
    if (!discreteGPUs.empty())
    {
        uint32_t selectedIndex = 0;
#if DONUT_WITH_STREAMLINE
        // Auto select best adapter for streamline features
        if (adapterIndex < 0)
            selectedIndex = StreamlineIntegration::Get().FindBestAdapterVulkan(discreteGPUs);
#endif

        m_VulkanPhysicalDevice = discreteGPUs[selectedIndex];
        return true;
    }

    if (!otherGPUs.empty())
    {
        uint32_t selectedIndex = 0;
#if DONUT_WITH_STREAMLINE
        // Auto select best adapter for streamline features
        if (adapterIndex < 0)
            selectedIndex = StreamlineIntegration::Get().FindBestAdapterVulkan(otherGPUs);
#endif
        m_VulkanPhysicalDevice = otherGPUs[selectedIndex];
        return true;
    }

    log::error("%s", errorStream.str().c_str());

    return false;
}

bool DeviceManager_VK::findQueueFamilies(vk::PhysicalDevice physicalDevice)
{
    auto props = physicalDevice.getQueueFamilyProperties();

    for(int i = 0; i < int(props.size()); i++)
    {
        const auto& queueFamily = props[i];

        if (m_GraphicsQueueFamily == -1)
        {
            if (queueFamily.queueCount > 0 &&
                (queueFamily.queueFlags & vk::QueueFlagBits::eGraphics))
            {
                m_GraphicsQueueFamily = i;
            }
        }

        if (m_ComputeQueueFamily == -1)
        {
            if (queueFamily.queueCount > 0 &&
                (queueFamily.queueFlags & vk::QueueFlagBits::eCompute) &&
                !(queueFamily.queueFlags & vk::QueueFlagBits::eGraphics))
            {
                m_ComputeQueueFamily = i;
            }
        }

        if (m_TransferQueueFamily == -1)
        {
            if (queueFamily.queueCount > 0 &&
                (queueFamily.queueFlags & vk::QueueFlagBits::eTransfer) && 
                !(queueFamily.queueFlags & vk::QueueFlagBits::eCompute) &&
                !(queueFamily.queueFlags & vk::QueueFlagBits::eGraphics))
            {
                m_TransferQueueFamily = i;
            }
        }

        if (m_PresentQueueFamily == -1)
        {
            if (queueFamily.queueCount > 0 &&
                glfwGetPhysicalDevicePresentationSupport(m_VulkanInstance, physicalDevice, i))
            {
                m_PresentQueueFamily = i;
            }
        }
    }

    if (m_GraphicsQueueFamily == -1 || 
        (m_PresentQueueFamily == -1 && !m_DeviceParams.headlessDevice) ||
        (m_ComputeQueueFamily == -1 && m_DeviceParams.enableComputeQueue) || 
        (m_TransferQueueFamily == -1 && m_DeviceParams.enableCopyQueue))
    {
        return false;
    }

    return true;
}

bool DeviceManager_VK::createDevice()
{
    // figure out which optional extensions are supported
    auto deviceExtensions = m_VulkanPhysicalDevice.enumerateDeviceExtensionProperties();
    for(const auto& ext : deviceExtensions)
    {
        const std::string name = ext.extensionName;
        if (optionalExtensions.device.find(name) != optionalExtensions.device.end())
        {
            if (name == VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME && m_DeviceParams.headlessDevice)
                continue;

            enabledExtensions.device.insert(name);
        }

        if (m_DeviceParams.enableRayTracingExtensions && m_RayTracingExtensions.find(name) != m_RayTracingExtensions.end())
        {
            enabledExtensions.device.insert(name);
        }
    }

    if (!m_DeviceParams.headlessDevice)
    {
        enabledExtensions.device.insert(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    const vk::PhysicalDeviceProperties physicalDeviceProperties = m_VulkanPhysicalDevice.getProperties();
    m_RendererString = std::string(physicalDeviceProperties.deviceName.data());

    bool accelStructSupported = false;
    bool rayPipelineSupported = false;
    bool rayQuerySupported = false;
    bool meshletsSupported = false;
    bool vrsSupported = false;
    bool interlockSupported = false;
    bool barycentricSupported = false;
    bool synchronization2Supported = false;
    bool maintenance4Supported = false;
    bool aftermathSupported = false;
    bool clusterAccelerationStructureSupported = false;
    bool mutableDescriptorTypeSupported = false;
    bool linearSweptSpheresSupported = false;

    log::message(m_DeviceParams.infoLogSeverity, "Enabled Vulkan device extensions:");
    for (const auto& ext : enabledExtensions.device)
    {
        log::message(m_DeviceParams.infoLogSeverity, "    %s", ext.c_str());

        if (ext == VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
            accelStructSupported = true;
        else if (ext == VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)
            rayPipelineSupported = true;
        else if (ext == VK_KHR_RAY_QUERY_EXTENSION_NAME)
            rayQuerySupported = true;
        else if (ext == VK_NV_MESH_SHADER_EXTENSION_NAME)
            meshletsSupported = true;
        else if (ext == VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)
            vrsSupported = true;
        else if (ext == VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME)
            interlockSupported = true;
        else if (ext == VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME)
            barycentricSupported = true;
        else if (ext == VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)
            synchronization2Supported = true;
        else if (ext == VK_KHR_MAINTENANCE_4_EXTENSION_NAME)
            maintenance4Supported = true;
        else if (ext == VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME)
            m_SwapChainMutableFormatSupported = true;
        else if (ext == VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME)
            aftermathSupported = true;
        else if (ext == VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME)
            clusterAccelerationStructureSupported = true;
        else if (ext == VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME)
            mutableDescriptorTypeSupported = true;
        else if (ext == VK_NV_RAY_TRACING_LINEAR_SWEPT_SPHERES_EXTENSION_NAME)
            linearSweptSpheresSupported = true;
    }

#define APPEND_EXTENSION(condition, desc) if (condition) { (desc).pNext = pNext; pNext = &(desc); }  // NOLINT(cppcoreguidelines-macro-usage)
    void* pNext = nullptr;

    vk::PhysicalDeviceFeatures2 physicalDeviceFeatures2;
    // Determine support for Buffer Device Address, the Vulkan 1.2 way
    auto bufferDeviceAddressFeatures = vk::PhysicalDeviceBufferDeviceAddressFeatures();
    // Determine support for maintenance4
    auto maintenance4Features = vk::PhysicalDeviceMaintenance4Features();
    // Determine support for aftermath
    auto aftermathPhysicalFeatures = vk::PhysicalDeviceDiagnosticsConfigFeaturesNV();

    // Put the user-provided extension structure at the end of the chain
    pNext = m_DeviceParams.physicalDeviceFeatures2Extensions;
    APPEND_EXTENSION(true, bufferDeviceAddressFeatures);
    APPEND_EXTENSION(maintenance4Supported, maintenance4Features);
    APPEND_EXTENSION(aftermathSupported, aftermathPhysicalFeatures);

    physicalDeviceFeatures2.pNext = pNext;
    m_VulkanPhysicalDevice.getFeatures2(&physicalDeviceFeatures2);

    std::unordered_set<int> uniqueQueueFamilies = {
        m_GraphicsQueueFamily };

    if (!m_DeviceParams.headlessDevice)
        uniqueQueueFamilies.insert(m_PresentQueueFamily);

    if (m_DeviceParams.enableComputeQueue)
        uniqueQueueFamilies.insert(m_ComputeQueueFamily);

    if (m_DeviceParams.enableCopyQueue)
        uniqueQueueFamilies.insert(m_TransferQueueFamily);

    float priority = 1.f;
    std::vector<vk::DeviceQueueCreateInfo> queueDesc;
    queueDesc.reserve(uniqueQueueFamilies.size());
    for(int queueFamily : uniqueQueueFamilies)
    {
        queueDesc.push_back(vk::DeviceQueueCreateInfo()
                                .setQueueFamilyIndex(queueFamily)
                                .setQueueCount(1)
                                .setPQueuePriorities(&priority));
    }

    auto accelStructFeatures = vk::PhysicalDeviceAccelerationStructureFeaturesKHR()
        .setAccelerationStructure(true);
    auto rayPipelineFeatures = vk::PhysicalDeviceRayTracingPipelineFeaturesKHR()
        .setRayTracingPipeline(true)
        .setRayTraversalPrimitiveCulling(true);
    auto rayQueryFeatures = vk::PhysicalDeviceRayQueryFeaturesKHR()
        .setRayQuery(true);
    auto meshletFeatures = vk::PhysicalDeviceMeshShaderFeaturesNV()
        .setTaskShader(true)
        .setMeshShader(true);
    auto interlockFeatures = vk::PhysicalDeviceFragmentShaderInterlockFeaturesEXT()
        .setFragmentShaderPixelInterlock(true);
    auto barycentricFeatures = vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR()
        .setFragmentShaderBarycentric(true);
    auto vrsFeatures = vk::PhysicalDeviceFragmentShadingRateFeaturesKHR()
        .setPipelineFragmentShadingRate(true)
        .setPrimitiveFragmentShadingRate(true)
        .setAttachmentFragmentShadingRate(true);
    auto vulkan13features = vk::PhysicalDeviceVulkan13Features()
        .setDynamicRendering(true)
        .setSynchronization2(synchronization2Supported)
        .setMaintenance4(maintenance4Features.maintenance4);
#if DONUT_WITH_AFTERMATH
    auto aftermathFeatures = vk::DeviceDiagnosticsConfigCreateInfoNV()
        .setFlags(vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableResourceTracking
            | vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderDebugInfo
            | vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderErrorReporting);
#endif
    auto clusterAccelerationStructureFeatures = vk::PhysicalDeviceClusterAccelerationStructureFeaturesNV()
        .setClusterAccelerationStructure(true);
    auto mutableDescriptorTypeFeatures = vk::PhysicalDeviceMutableDescriptorTypeFeaturesEXT()
        .setMutableDescriptorType(true);
    auto dynamicRenderingFeatures = vk::PhysicalDeviceDynamicRenderingFeatures()
        .setDynamicRendering(true);
    auto linearSweptSpheresFeatures = vk::PhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV()
        .setSpheres(true)
        .setLinearSweptSpheres(true);
    
    pNext = nullptr;
    APPEND_EXTENSION(accelStructSupported, accelStructFeatures)
    APPEND_EXTENSION(rayPipelineSupported, rayPipelineFeatures)
    APPEND_EXTENSION(rayQuerySupported, rayQueryFeatures)
    APPEND_EXTENSION(meshletsSupported, meshletFeatures)
    APPEND_EXTENSION(vrsSupported, vrsFeatures)
    APPEND_EXTENSION(interlockSupported, interlockFeatures)
    APPEND_EXTENSION(barycentricSupported, barycentricFeatures)
    APPEND_EXTENSION(clusterAccelerationStructureSupported, clusterAccelerationStructureFeatures)
    APPEND_EXTENSION(mutableDescriptorTypeSupported, mutableDescriptorTypeFeatures)
    APPEND_EXTENSION(physicalDeviceProperties.apiVersion >= VK_API_VERSION_1_3, vulkan13features)
    APPEND_EXTENSION(physicalDeviceProperties.apiVersion < VK_API_VERSION_1_3 && maintenance4Supported, maintenance4Features)
    APPEND_EXTENSION(physicalDeviceProperties.apiVersion < VK_API_VERSION_1_3, dynamicRenderingFeatures)
    APPEND_EXTENSION(linearSweptSpheresSupported, linearSweptSpheresFeatures)
    
#if DONUT_WITH_AFTERMATH
    if (aftermathPhysicalFeatures.diagnosticsConfig && m_DeviceParams.enableAftermath)
        APPEND_EXTENSION(aftermathSupported, aftermathFeatures);
#endif
#undef APPEND_EXTENSION

    auto deviceFeatures = vk::PhysicalDeviceFeatures()
        .setShaderImageGatherExtended(true)
        .setSamplerAnisotropy(true)
        .setTessellationShader(true)
        .setTextureCompressionBC(true)
        .setGeometryShader(true)
        .setImageCubeArray(true)
        .setShaderInt16(true)
        .setFillModeNonSolid(true)
        .setFragmentStoresAndAtomics(true)
        .setDualSrcBlend(true)
        .setVertexPipelineStoresAndAtomics(true)
        .setShaderInt64(true)
        .setShaderStorageImageWriteWithoutFormat(true)
        .setShaderStorageImageReadWithoutFormat(true);

    // Add a Vulkan 1.1 structure with default settings to make it easier for apps to modify them
    auto vulkan11features = vk::PhysicalDeviceVulkan11Features()
        .setStorageBuffer16BitAccess(true)
        .setPNext(pNext);

    auto vulkan12features = vk::PhysicalDeviceVulkan12Features()
        .setDescriptorIndexing(true)
        .setRuntimeDescriptorArray(true)
        .setDescriptorBindingPartiallyBound(true)
        .setDescriptorBindingVariableDescriptorCount(true)
        .setTimelineSemaphore(true)
        .setShaderSampledImageArrayNonUniformIndexing(true)
        .setBufferDeviceAddress(bufferDeviceAddressFeatures.bufferDeviceAddress)
        .setShaderSubgroupExtendedTypes(true)
        .setScalarBlockLayout(true)
        .setPNext(&vulkan11features);

    auto extVec = stringSetToVector(enabledExtensions.device);

    auto deviceDesc = vk::DeviceCreateInfo()
        .setPQueueCreateInfos(queueDesc.data())
        .setQueueCreateInfoCount(uint32_t(queueDesc.size()))
        .setPEnabledFeatures(&deviceFeatures)
        .setEnabledExtensionCount(uint32_t(extVec.size()))
        .setPpEnabledExtensionNames(extVec.data())
        .setPNext(&vulkan12features);

    if (m_DeviceParams.deviceCreateInfoCallback)
        m_DeviceParams.deviceCreateInfoCallback(deviceDesc);
    
    const vk::Result res = m_VulkanPhysicalDevice.createDevice(&deviceDesc, nullptr, &m_VulkanDevice);
    if (res != vk::Result::eSuccess)
    {
        log::error("Failed to create a Vulkan physical device, error code = %s", nvrhi::vulkan::resultToString(VkResult(res)));
        return false;
    }

    m_VulkanDevice.getQueue(m_GraphicsQueueFamily, kGraphicsQueueIndex, &m_GraphicsQueue);
    if (m_DeviceParams.enableComputeQueue)
        m_VulkanDevice.getQueue(m_ComputeQueueFamily, kComputeQueueIndex, &m_ComputeQueue);
    if (m_DeviceParams.enableCopyQueue)
        m_VulkanDevice.getQueue(m_TransferQueueFamily, kTransferQueueIndex, &m_TransferQueue);
    if (!m_DeviceParams.headlessDevice)
        m_VulkanDevice.getQueue(m_PresentQueueFamily, kPresentQueueIndex, &m_PresentQueue);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_VulkanDevice);

    // remember the bufferDeviceAddress feature enablement
    m_BufferDeviceAddressSupported = vulkan12features.bufferDeviceAddress;

    log::message(m_DeviceParams.infoLogSeverity, "Created Vulkan device: %s", m_RendererString.c_str());

    return true;
}

bool DeviceManager_VK::createWindowSurface()
{
    const VkResult res = glfwCreateWindowSurface(m_VulkanInstance, m_Window, nullptr, (VkSurfaceKHR *)&m_WindowSurface);
    if (res != VK_SUCCESS)
    {
        log::error("Failed to create a GLFW window surface, error code = %s", nvrhi::vulkan::resultToString(res));
        return false;
    }

    return true;
}

void DeviceManager_VK::destroySwapChain()
{
    if (m_VulkanDevice)
    {
        m_VulkanDevice.waitIdle();
    }

    if (m_SwapChain)
    {
        m_VulkanDevice.destroySwapchainKHR(m_SwapChain);
        m_SwapChain = nullptr;
    }

    m_SwapChainImages.clear();
}

bool DeviceManager_VK::createSwapChain()
{
    destroySwapChain();

    m_SwapChainFormat = {
        vk::Format(nvrhi::vulkan::convertFormat(m_DeviceParams.swapChainFormat)),
        vk::ColorSpaceKHR::eSrgbNonlinear
    };

    vk::Extent2D extent = vk::Extent2D(m_DeviceParams.backBufferWidth, m_DeviceParams.backBufferHeight);

    std::unordered_set<uint32_t> uniqueQueues = {
        uint32_t(m_GraphicsQueueFamily),
        uint32_t(m_PresentQueueFamily) };
    
    std::vector<uint32_t> queues = setToVector(uniqueQueues);

    const bool enableSwapChainSharing = queues.size() > 1;

    auto desc = vk::SwapchainCreateInfoKHR()
                    .setSurface(m_WindowSurface)
                    .setMinImageCount(m_DeviceParams.swapChainBufferCount)
                    .setImageFormat(m_SwapChainFormat.format)
                    .setImageColorSpace(m_SwapChainFormat.colorSpace)
                    .setImageExtent(extent)
                    .setImageArrayLayers(1)
                    .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled)
                    .setImageSharingMode(enableSwapChainSharing ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive)
                    .setFlags(m_SwapChainMutableFormatSupported ? vk::SwapchainCreateFlagBitsKHR::eMutableFormat : vk::SwapchainCreateFlagBitsKHR(0))
                    .setQueueFamilyIndexCount(enableSwapChainSharing ? uint32_t(queues.size()) : 0)
                    .setPQueueFamilyIndices(enableSwapChainSharing ? queues.data() : nullptr)
                    .setPreTransform(vk::SurfaceTransformFlagBitsKHR::eIdentity)
                    .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
                    .setPresentMode(m_DeviceParams.vsyncEnabled ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate)
                    .setClipped(true)
                    .setOldSwapchain(nullptr);
    
    std::vector<vk::Format> imageFormats = { m_SwapChainFormat.format };
    switch(m_SwapChainFormat.format)
    {
        case vk::Format::eR8G8B8A8Unorm:
            imageFormats.push_back(vk::Format::eR8G8B8A8Srgb);
            break;
        case vk::Format::eR8G8B8A8Srgb:
            imageFormats.push_back(vk::Format::eR8G8B8A8Unorm);
            break;
        case vk::Format::eB8G8R8A8Unorm:
            imageFormats.push_back(vk::Format::eB8G8R8A8Srgb);
            break;
        case vk::Format::eB8G8R8A8Srgb:
            imageFormats.push_back(vk::Format::eB8G8R8A8Unorm);
            break;
        default:
            break;
    }

    auto imageFormatListCreateInfo = vk::ImageFormatListCreateInfo()
        .setViewFormats(imageFormats);

    if (m_SwapChainMutableFormatSupported)
        desc.pNext = &imageFormatListCreateInfo;

    const vk::Result res = m_VulkanDevice.createSwapchainKHR(&desc, nullptr, &m_SwapChain);
    if (res != vk::Result::eSuccess)
    {
        log::error("Failed to create a Vulkan swap chain, error code = %s", nvrhi::vulkan::resultToString(VkResult(res)));
        return false;
    }

    // retrieve swap chain images
    auto images = m_VulkanDevice.getSwapchainImagesKHR(m_SwapChain);
    for(auto image : images)
    {
        SwapChainImage sci;
        sci.image = image;
        
        nvrhi::TextureDesc textureDesc;
        textureDesc.width = m_DeviceParams.backBufferWidth;
        textureDesc.height = m_DeviceParams.backBufferHeight;
        textureDesc.format = m_DeviceParams.swapChainFormat;
        textureDesc.debugName = "Swap chain image";
        textureDesc.initialState = nvrhi::ResourceStates::Present;
        textureDesc.keepInitialState = true;
        textureDesc.isRenderTarget = true;

        sci.rhiHandle = m_NvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(sci.image), textureDesc);
        m_SwapChainImages.push_back(sci);
    }

    m_SwapChainIndex = 0;

    return true;
}

#define CHECK(a) if (!(a)) { return false; }

bool DeviceManager_VK::CreateInstanceInternal()
{
#if DONUT_WITH_STREAMLINE
    StreamlineIntegration::Get().InitializePreDevice(nvrhi::GraphicsAPI::VULKAN, m_DeviceParams.streamlineAppId, m_DeviceParams.checkStreamlineSignature, m_DeviceParams.enableStreamlineLog);
#endif

    if (m_DeviceParams.enableDebugRuntime)
    {
        enabledExtensions.instance.insert("VK_EXT_debug_report");
        enabledExtensions.layers.insert("VK_LAYER_KHRONOS_validation");
    }

#if DONUT_WITH_STREAMLINE
    m_DeviceParams.vulkanLibraryName = "sl.interposer.dll";
#endif

    m_dynamicLoader = std::make_unique<VulkanDynamicLoader>(m_DeviceParams.vulkanLibraryName);

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        m_dynamicLoader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    return createInstance();
}

bool DeviceManager_VK::EnumerateAdapters(std::vector<AdapterInfo>& outAdapters)
{
    if (!m_VulkanInstance)
        return false;

    std::vector<vk::PhysicalDevice> devices = m_VulkanInstance.enumeratePhysicalDevices();
    outAdapters.clear();

    for (auto physicalDevice : devices)
    {
        vk::PhysicalDeviceProperties2 properties2;
        vk::PhysicalDeviceIDProperties idProperties;
        properties2.pNext = &idProperties;
        physicalDevice.getProperties2(&properties2);

        auto const& properties = properties2.properties;
        
        AdapterInfo adapterInfo;
        adapterInfo.name = properties.deviceName.data();
        adapterInfo.vendorID = properties.vendorID;
        adapterInfo.deviceID = properties.deviceID;
        adapterInfo.vkPhysicalDevice = physicalDevice;
        adapterInfo.dedicatedVideoMemory = 0;

        AdapterInfo::UUID uuid;
        static_assert(uuid.size() == idProperties.deviceUUID.size());
        memcpy(uuid.data(), idProperties.deviceUUID.data(), uuid.size());
        adapterInfo.uuid = uuid;

        if (idProperties.deviceLUIDValid)
        {
            AdapterInfo::LUID luid;
            static_assert(luid.size() == idProperties.deviceLUID.size());
            memcpy(luid.data(), idProperties.deviceLUID.data(), luid.size());
            adapterInfo.luid = luid;
        }

        // Go through the memory types to figure out the amount of VRAM on this physical device.
        vk::PhysicalDeviceMemoryProperties memoryProperties = physicalDevice.getMemoryProperties();
        for (uint32_t heapIndex = 0; heapIndex < memoryProperties.memoryHeapCount; ++heapIndex)
        {
            vk::MemoryHeap const& heap = memoryProperties.memoryHeaps[heapIndex];
            if (heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal)
            {
                adapterInfo.dedicatedVideoMemory += heap.size;
            }
        }

        outAdapters.push_back(std::move(adapterInfo));
    }

    return true;
}

bool DeviceManager_VK::CreateDevice()
{
    if (m_DeviceParams.enableDebugRuntime)
    {
        installDebugCallback();
    }

    // add device extensions requested by the user
    for (const std::string& name : m_DeviceParams.requiredVulkanDeviceExtensions)
    {
        enabledExtensions.device.insert(name);
    }
    for (const std::string& name : m_DeviceParams.optionalVulkanDeviceExtensions)
    {
        optionalExtensions.device.insert(name);
    }

    if (!m_DeviceParams.headlessDevice)
    {
        // Need to adjust the swap chain format before creating the device because it affects physical device selection
        if (m_DeviceParams.swapChainFormat == nvrhi::Format::SRGBA8_UNORM)
            m_DeviceParams.swapChainFormat = nvrhi::Format::SBGRA8_UNORM;
        else if (m_DeviceParams.swapChainFormat == nvrhi::Format::RGBA8_UNORM)
            m_DeviceParams.swapChainFormat = nvrhi::Format::BGRA8_UNORM;

        CHECK(createWindowSurface())
    }
    CHECK(pickPhysicalDevice())
    CHECK(findQueueFamilies(m_VulkanPhysicalDevice))
    CHECK(createDevice())

    auto vecInstanceExt = stringSetToVector(enabledExtensions.instance);
    auto vecLayers = stringSetToVector(enabledExtensions.layers);
    auto vecDeviceExt = stringSetToVector(enabledExtensions.device);

    nvrhi::vulkan::DeviceDesc deviceDesc;
    deviceDesc.errorCB = &DefaultMessageCallback::GetInstance();
    deviceDesc.instance = m_VulkanInstance;
    deviceDesc.physicalDevice = m_VulkanPhysicalDevice;
    deviceDesc.device = m_VulkanDevice;
    deviceDesc.graphicsQueue = m_GraphicsQueue;
    deviceDesc.graphicsQueueIndex = m_GraphicsQueueFamily;
    if (m_DeviceParams.enableComputeQueue)
    {
        deviceDesc.computeQueue = m_ComputeQueue;
        deviceDesc.computeQueueIndex = m_ComputeQueueFamily;
    }
    if (m_DeviceParams.enableCopyQueue)
    {
        deviceDesc.transferQueue = m_TransferQueue;
        deviceDesc.transferQueueIndex = m_TransferQueueFamily;
    }
    deviceDesc.instanceExtensions = vecInstanceExt.data();
    deviceDesc.numInstanceExtensions = vecInstanceExt.size();
    deviceDesc.deviceExtensions = vecDeviceExt.data();
    deviceDesc.numDeviceExtensions = vecDeviceExt.size();
    deviceDesc.bufferDeviceAddressSupported = m_BufferDeviceAddressSupported;
#if DONUT_WITH_AFTERMATH
    deviceDesc.aftermathEnabled = m_DeviceParams.enableAftermath;
#endif
    deviceDesc.vulkanLibraryName = m_DeviceParams.vulkanLibraryName;
    deviceDesc.logBufferLifetime = m_DeviceParams.logBufferLifetime;

    m_NvrhiDevice = nvrhi::vulkan::createDevice(deviceDesc);

    if (m_DeviceParams.enableNvrhiValidationLayer)
    {
        m_ValidationLayer = nvrhi::validation::createValidationLayer(m_NvrhiDevice);
    }

#if DONUT_WITH_STREAMLINE
    StreamlineIntegration::VulkanInfo vulkanInfo;
    vulkanInfo.vkDevice = m_VulkanDevice;
    vulkanInfo.vkInstance = m_VulkanInstance;
    vulkanInfo.vkPhysicalDevice = m_VulkanPhysicalDevice;
    vulkanInfo.computeQueueIndex = kComputeQueueIndex;
    vulkanInfo.computeQueueFamily = m_ComputeQueueFamily;
    vulkanInfo.graphicsQueueIndex = kGraphicsQueueIndex;
    vulkanInfo.graphicsQueueFamily = m_GraphicsQueueFamily;
    
    StreamlineIntegration::Get().InitializeDeviceVK(m_NvrhiDevice, vulkanInfo);
#endif

    return true;
}

bool DeviceManager_VK::CreateSwapChain()
{
    CHECK(createSwapChain())

    size_t const numPresentSemaphores = m_SwapChainImages.size();
    m_PresentSemaphores.reserve(numPresentSemaphores);
    for (uint32_t i = 0; i < numPresentSemaphores; ++i)
    {
        m_PresentSemaphores.push_back(m_VulkanDevice.createSemaphore(vk::SemaphoreCreateInfo()));
    }

    size_t const numAcquireSemaphores = std::max(size_t(m_DeviceParams.maxFramesInFlight),
        m_SwapChainImages.size());
    m_AcquireSemaphores.reserve(numAcquireSemaphores);
    for (uint32_t i = 0; i < numAcquireSemaphores; ++i)
    {
        m_AcquireSemaphores.push_back(m_VulkanDevice.createSemaphore(vk::SemaphoreCreateInfo()));
    }

    return true;
}
#undef CHECK

void DeviceManager_VK::DestroyDeviceAndSwapChain()
{
    destroySwapChain();

    for (auto& semaphore : m_PresentSemaphores)
    {
        if (semaphore)
        {
            m_VulkanDevice.destroySemaphore(semaphore);
            semaphore = vk::Semaphore();
        }
    }

    for (auto& semaphore : m_AcquireSemaphores)
    {
        if (semaphore)
        {
            m_VulkanDevice.destroySemaphore(semaphore);
            semaphore = vk::Semaphore();
        }
    }

    m_NvrhiDevice = nullptr;
    m_ValidationLayer = nullptr;
    m_RendererString.clear();
    
    if (m_VulkanDevice)
    {
        m_VulkanDevice.destroy();
        m_VulkanDevice = nullptr;
    }

    if (m_WindowSurface)
    {
        assert(m_VulkanInstance);
        m_VulkanInstance.destroySurfaceKHR(m_WindowSurface);
        m_WindowSurface = nullptr;
    }

    if (m_DebugReportCallback)
    {
        m_VulkanInstance.destroyDebugReportCallbackEXT(m_DebugReportCallback);
    }

    if (m_VulkanInstance)
    {
        m_VulkanInstance.destroy();
        m_VulkanInstance = nullptr;
    }
}

bool DeviceManager_VK::BeginFrame()
{
    const auto& semaphore = m_AcquireSemaphores[m_AcquireSemaphoreIndex];

    vk::Result res;

    int const maxAttempts = 3;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        res = m_VulkanDevice.acquireNextImageKHR(
            m_SwapChain,
            std::numeric_limits<uint64_t>::max(), // timeout
            semaphore,
            vk::Fence(),
            &m_SwapChainIndex);

        if ((res == vk::Result::eErrorOutOfDateKHR || res == vk::Result::eSuboptimalKHR) && attempt < maxAttempts)
        {
            BackBufferResizing();
            auto surfaceCaps = m_VulkanPhysicalDevice.getSurfaceCapabilitiesKHR(m_WindowSurface);

            m_DeviceParams.backBufferWidth = surfaceCaps.currentExtent.width;
            m_DeviceParams.backBufferHeight = surfaceCaps.currentExtent.height;

            ResizeSwapChain();
            BackBufferResized();
        }
        else
            break;
    }

    m_AcquireSemaphoreIndex = (m_AcquireSemaphoreIndex + 1) % m_AcquireSemaphores.size();

    if (res == vk::Result::eSuccess || res == vk::Result::eSuboptimalKHR) // Suboptimal is considered a success
    {
        // Schedule the wait. The actual wait operation will be submitted when the app executes any command list.
        m_NvrhiDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, semaphore, 0);
        return true;
    }

    return false;
}

bool DeviceManager_VK::Present()
{
    const auto& semaphore = m_PresentSemaphores[m_SwapChainIndex];

    m_NvrhiDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, semaphore, 0);

    // NVRHI buffers the semaphores and signals them when something is submitted to a queue.
    // Call 'executeCommandLists' with no command lists to actually signal the semaphore.
    m_NvrhiDevice->executeCommandLists(nullptr, 0);

    vk::PresentInfoKHR info = vk::PresentInfoKHR()
                                .setWaitSemaphoreCount(1)
                                .setPWaitSemaphores(&semaphore)
                                .setSwapchainCount(1)
                                .setPSwapchains(&m_SwapChain)
                                .setPImageIndices(&m_SwapChainIndex);

    const vk::Result res = m_PresentQueue.presentKHR(&info);
    if (!(res == vk::Result::eSuccess || res == vk::Result::eErrorOutOfDateKHR || res == vk::Result::eSuboptimalKHR))
    {
        return false;
    }

#ifndef _WIN32
    if (m_DeviceParams.vsyncEnabled || m_DeviceParams.enableDebugRuntime)
    {
        // according to vulkan-tutorial.com, "the validation layer implementation expects
        // the application to explicitly synchronize with the GPU"
        m_PresentQueue.waitIdle();
    }
#endif

    while (m_FramesInFlight.size() >= m_DeviceParams.maxFramesInFlight)
    {
        auto query = m_FramesInFlight.front();
        m_FramesInFlight.pop();

        m_NvrhiDevice->waitEventQuery(query);

        m_QueryPool.push_back(query);
    }

    nvrhi::EventQueryHandle query;
    if (!m_QueryPool.empty())
    {
        query = m_QueryPool.back();
        m_QueryPool.pop_back();
    }
    else
    {
        query = m_NvrhiDevice->createEventQuery();
    }

    m_NvrhiDevice->resetEventQuery(query);
    m_NvrhiDevice->setEventQuery(query, nvrhi::CommandQueue::Graphics);
    m_FramesInFlight.push(query);
    return true;
}

DeviceManager *DeviceManager::CreateVK()
{
    return new DeviceManager_VK();
}
