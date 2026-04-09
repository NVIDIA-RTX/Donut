/*
* Copyright (c) 2021-2025, NVIDIA CORPORATION. All rights reserved.
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

#if DONUT_WITH_DLSS && DONUT_WITH_VULKAN

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers_vk.h>

#include <donut/render/DLSS.h>
#include <donut/engine/View.h>
#include <donut/core/log.h>
#include <nvrhi/vulkan.h>

using namespace donut;
using namespace donut::render;

static void NVSDK_CONV NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent)
{
    log::info("NGX: %s", message);
}

class DLSS_VK : public DLSS
{
public:
    DLSS_VK(nvrhi::IDevice* device, donut::engine::ShaderFactory& shaderFactory,
        std::string const& directoryWithExecutable, uint32_t applicationID)
        : DLSS(device, shaderFactory)
    {
        VkInstance vkInstance = device->getNativeObject(nvrhi::ObjectTypes::VK_Instance);
        VkPhysicalDevice vkPhysicalDevice = device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice);
        VkDevice vkDevice= device->getNativeObject(nvrhi::ObjectTypes::VK_Device);

        std::wstring executablePathW;
        executablePathW.assign(directoryWithExecutable.begin(), directoryWithExecutable.end());

        NVSDK_NGX_FeatureCommonInfo featureCommonInfo = {};
        featureCommonInfo.LoggingInfo.LoggingCallback = NgxLogCallback;
        featureCommonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
        featureCommonInfo.LoggingInfo.DisableOtherLoggingSinks = true;

        NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init(applicationID,
            executablePathW.c_str(), vkInstance, vkPhysicalDevice, vkDevice, nullptr, nullptr, &featureCommonInfo);

        if (result != NVSDK_NGX_Result_Success)
        {
            log::warning("Cannot initialize NGX, Result = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            return;
        }
        
        result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_parameters);

        if (result != NVSDK_NGX_Result_Success)
            return;

        // DLSS
        {
            int available = 0;
            result = m_parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
            m_dlssSupported = available && result == NVSDK_NGX_Result_Success;
            if (!m_dlssSupported)
            {
                result = NVSDK_NGX_Result_Fail;
                NVSDK_NGX_Parameter_GetI(m_parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, (int*)&result);
                log::warning("NVIDIA DLSS is not available on this system, FeatureInitResult = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            }
        }

        // DLSS RR
        {
            int available = 0;
            result = m_parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
            m_rayReconstructionSupported = available && result == NVSDK_NGX_Result_Success;
            if (!m_rayReconstructionSupported)
            {
                result = NVSDK_NGX_Result_Fail;
                NVSDK_NGX_Parameter_GetI(m_parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, (int*)&result);
                log::warning("NVIDIA DLSSRR is not available on this system, FeatureInitResult = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            }
        }
    }

    void Init(const InitParameters& params) override
    {
        if (params.useRayReconstruction && !m_rayReconstructionSupported)
            return;
        else if (!m_dlssSupported)
            return;

        if (m_initParameters.inputWidth == params.inputWidth && m_initParameters.inputHeight == params.inputHeight &&
            m_initParameters.outputWidth == params.outputWidth && m_initParameters.outputHeight == params.outputHeight &&
            m_initParameters.useLinearDepth == params.useLinearDepth &&
            m_initParameters.useAutoExposure == params.useAutoExposure &&
            m_initParameters.useRayReconstruction == params.useRayReconstruction)
            return;

        if (m_dlssHandle)
        {
            m_device->waitForIdle();
            NVSDK_NGX_VULKAN_ReleaseFeature(m_dlssHandle);
            m_dlssHandle = nullptr;
            m_dlssInitialized = false;
            m_rayReconstructionInitialized = false;
        }

        m_featureCommandList->open();
        VkCommandBuffer vkCmdBuf = m_featureCommandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);

        m_parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        m_parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        m_parameters->Set(NVSDK_NGX_Parameter_Width, params.inputWidth);
        m_parameters->Set(NVSDK_NGX_Parameter_Height, params.inputHeight);
        m_parameters->Set(NVSDK_NGX_Parameter_OutWidth, params.outputWidth);
        m_parameters->Set(NVSDK_NGX_Parameter_OutHeight, params.outputHeight);

        int inFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        inFeatureCreateFlags |= params.useLinearDepth ? 0 : NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, inFeatureCreateFlags);
        m_parameters->Set(NVSDK_NGX_Parameter_Use_HW_Depth, params.useLinearDepth ? NVSDK_NGX_DLSS_Depth_Type_Linear : NVSDK_NGX_DLSS_Depth_Type_HW);

        NVSDK_NGX_Result result;
        if (params.useRayReconstruction)
        {
            m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
            m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_DLSS_Roughness_Mode_Packed);

            result = NVSDK_NGX_VULKAN_CreateFeature(vkCmdBuf, NVSDK_NGX_Feature_RayReconstruction, m_parameters, &m_dlssHandle);
        }
        else
        {
            result = NVSDK_NGX_VULKAN_CreateFeature(vkCmdBuf, NVSDK_NGX_Feature_SuperSampling, m_parameters, &m_dlssHandle);
        }

        m_featureCommandList->close();
        m_device->executeCommandList(m_featureCommandList);

        if (result != NVSDK_NGX_Result_Success)
        {
            log::warning("Failed to create a DLSS feautre, Result = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            return;
        }

        if (params.useRayReconstruction)
            m_rayReconstructionInitialized = true;
        else
            m_dlssInitialized = true;

        m_initParameters = params;
    }

    static void FillTextureResource(NVSDK_NGX_Resource_VK& resource, nvrhi::ITexture* texture)
    {
        const nvrhi::TextureDesc& desc = texture->getDesc();
        resource.ReadWrite = desc.isUAV;
        resource.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;

        auto& viewInfo = resource.Resource.ImageViewInfo;
        viewInfo.Image = texture->getNativeObject(nvrhi::ObjectTypes::VK_Image);
        viewInfo.ImageView = texture->getNativeView(nvrhi::ObjectTypes::VK_ImageView);
        viewInfo.Format = VkFormat(nvrhi::vulkan::convertFormat(desc.format));
        viewInfo.Width = desc.width;
        viewInfo.Height = desc.height;
        viewInfo.SubresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.SubresourceRange.baseArrayLayer = 0;
        viewInfo.SubresourceRange.layerCount = 1;
        viewInfo.SubresourceRange.baseMipLevel = 0;
        viewInfo.SubresourceRange.levelCount = 1;
    }

    void Evaluate(
        nvrhi::ICommandList* commandList,
        const EvaluateParameters& params,
        const donut::engine::PlanarView& view) override
    {
        if (!m_dlssInitialized && !m_rayReconstructionInitialized)
            return;

        commandList->beginMarker(m_rayReconstructionInitialized ? "DLSS_RR" : "DLSS");

        bool const useExposureBuffer = params.exposureBuffer != nullptr && params.exposureScale != 0.f && !m_rayReconstructionInitialized;
        if (useExposureBuffer)
        {
            ComputeExposure(commandList, params.exposureBuffer, params.exposureScale);
        }

        VkCommandBuffer vkCmdBuf = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);

        NVSDK_NGX_Resource_VK inColorResource;
        NVSDK_NGX_Resource_VK outColorResource;
        NVSDK_NGX_Resource_VK depthResource;
        NVSDK_NGX_Resource_VK motionVectorResource;
        NVSDK_NGX_Resource_VK exposureResource;
        NVSDK_NGX_Resource_VK diffuseAlbedoResource;
        NVSDK_NGX_Resource_VK specularAlbedoResource;
        NVSDK_NGX_Resource_VK normalRoughnessResource;
        FillTextureResource(inColorResource, params.inputColorTexture);
        FillTextureResource(outColorResource, params.outputColorTexture);
        FillTextureResource(depthResource, params.depthTexture);
        FillTextureResource(motionVectorResource, params.motionVectorsTexture);
        if (m_rayReconstructionInitialized)
        {
            FillTextureResource(diffuseAlbedoResource, params.diffuseAlbedo);
            FillTextureResource(specularAlbedoResource, params.specularAlbedo);
            FillTextureResource(normalRoughnessResource, params.normalRoughness);
        }
        if (useExposureBuffer)
        {
            FillTextureResource(exposureResource, m_exposureTexture);
        }

        commandList->setTextureState(params.inputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(params.outputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(params.depthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(params.motionVectorsTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        if (m_rayReconstructionInitialized)
        {
            commandList->setTextureState(params.diffuseAlbedo, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(params.specularAlbedo, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(params.normalRoughness, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        }
        if (useExposureBuffer)
        {
            commandList->setTextureState(m_exposureTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        }
        commandList->commitBarriers();

        m_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, view.GetPixelOffset().x);
        m_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, view.GetPixelOffset().y);
        m_parameters->Set(NVSDK_NGX_Parameter_Reset, params.resetHistory);
        m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, view.GetViewExtent().width());
        m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, view.GetViewExtent().height());

        // Common buffers
        m_parameters->Set(NVSDK_NGX_Parameter_Color, &inColorResource);
        m_parameters->Set(NVSDK_NGX_Parameter_Output, &outColorResource);
        m_parameters->Set(NVSDK_NGX_Parameter_Depth, &depthResource);
        m_parameters->Set(NVSDK_NGX_Parameter_MotionVectors, &motionVectorResource);
        m_parameters->Set(NVSDK_NGX_Parameter_ExposureTexture, useExposureBuffer ? &exposureResource : nullptr);

        if (m_rayReconstructionInitialized)
        {
            m_parameters->Set(NVSDK_NGX_Parameter_DiffuseAlbedo, &diffuseAlbedoResource);
            m_parameters->Set(NVSDK_NGX_Parameter_SpecularAlbedo, &specularAlbedoResource);
            m_parameters->Set(NVSDK_NGX_Parameter_GBuffer_Normals, &normalRoughnessResource);
            m_parameters->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, &normalRoughnessResource);
        }

        NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_EvaluateFeature_C(vkCmdBuf, m_dlssHandle, m_parameters, NULL);

        commandList->clearState();

        commandList->endMarker();

        if (result != NVSDK_NGX_Result_Success)
        {
            log::warning("Failed to evaluate DLSS feature: 0x%08x", result);
            return;
        }
    }

    ~DLSS_VK() override
    {
        if (m_dlssHandle)
        {
            NVSDK_NGX_VULKAN_ReleaseFeature(m_dlssHandle);
            m_dlssHandle = nullptr;
        }

        if (m_parameters)
        {
            NVSDK_NGX_VULKAN_DestroyParameters(m_parameters);
            m_parameters = nullptr;
        }

        VkDevice vkDevice = m_device->getNativeObject(nvrhi::ObjectTypes::VK_Device);
        NVSDK_NGX_VULKAN_Shutdown1(vkDevice);
    }
};

std::unique_ptr<DLSS> DLSS::CreateVK(nvrhi::IDevice* device, donut::engine::ShaderFactory& shaderFactory,
    std::string const& directoryWithExecutable, uint32_t applicationID)
{
    return std::make_unique<DLSS_VK>(device, shaderFactory, directoryWithExecutable, applicationID);
}

void DLSS::GetRequiredVulkanExtensions(std::vector<std::string>& instanceExtensions, std::vector<std::string>& deviceExtensions)
{
    unsigned int instanceExtCount = 0;
    unsigned int deviceExtCount = 0;
    const char** pInstanceExtensions = nullptr;
    const char** pDeviceExtensions = nullptr;
    NVSDK_NGX_VULKAN_RequiredExtensions(&instanceExtCount, &pInstanceExtensions, &deviceExtCount, &pDeviceExtensions);

    for (unsigned int i = 0; i < instanceExtCount; i++)
    {
        instanceExtensions.push_back(pInstanceExtensions[i]);
    }

    for (unsigned int i = 0; i < deviceExtCount; i++)
    {
        // VK_EXT_buffer_device_address is incompatible with Vulkan 1.2 and causes a validation error
        if (!strcmp(pDeviceExtensions[i], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
            continue;

        deviceExtensions.push_back(pDeviceExtensions[i]);
    }
}

#endif
