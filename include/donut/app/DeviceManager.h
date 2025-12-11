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

#if DONUT_WITH_DX11 || DONUT_WITH_DX12
#include <DXGI.h>
#endif

#if DONUT_WITH_DX11
#include <d3d11.h>
#endif

#if DONUT_WITH_DX12
#include <d3d12.h>
#endif

#if DONUT_WITH_VULKAN
#include <nvrhi/vulkan.h>
#endif

#if DONUT_WITH_AFTERMATH
#include "AftermathCrashDump.h"
#endif

#if DONUT_WITH_STREAMLINE
#include <donut/app/StreamlineInterface.h>
#endif

#define GLFW_INCLUDE_NONE // Do not include any OpenGL headers
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif // _WIN32
#include <GLFW/glfw3native.h>
#include <nvrhi/nvrhi.h>
#include <donut/core/log.h>

#include <list>
#include <functional>
#include <optional>

namespace donut::app
{
    struct DefaultMessageCallback : public nvrhi::IMessageCallback
    {
        static DefaultMessageCallback& GetInstance();

        void message(nvrhi::MessageSeverity severity, const char* messageText) override;
    };

    struct InstanceParameters
    {
        bool enableDebugRuntime = false;
        bool enableWarningsAsErrors = false;
        bool enableGPUValidation = false; // Affects only DX12
        bool headlessDevice = false;
#if DONUT_WITH_AFTERMATH
        bool enableAftermath = false;
#endif
        bool logBufferLifetime = false;
        bool enableHeapDirectlyIndexed = false; // Allows ResourceDescriptorHeap on DX12

        // Enables per-monitor DPI scale support.
        //
        // If set to true, the app will receive DisplayScaleChanged() events on DPI change and can read
        // the scaling factors using GetDPIScaleInfo(...). The window may be resized when DPI changes if
        // DeviceCreationParameters::resizeWindowWithDisplayScale is true.
        //
        // If set to false, the app will see DPI scaling factors being 1.0 all the time, but the OS
        // may scale the contents of the window based on DPI.
        //
        // This field is located in InstanceParameters and not DeviceCreationParameters because it is needed
        // in the CreateInstance() function to override the glfwInit() behavior.
        bool enablePerMonitorDPI = false;

        // Severity of the information log messages from the device manager, like the device name or enabled extensions.
        log::Severity infoLogSeverity = log::Severity::Info;

#if DONUT_WITH_VULKAN
        // Allows overriding the Vulkan library name with something custom, useful for Streamline
        std::string vulkanLibraryName;
        
        std::vector<std::string> requiredVulkanInstanceExtensions;
        std::vector<std::string> requiredVulkanLayers;
        std::vector<std::string> optionalVulkanInstanceExtensions;
        std::vector<std::string> optionalVulkanLayers;
#endif

#if DONUT_WITH_STREAMLINE
        int streamlineAppId = 1; // default app id
        bool checkStreamlineSignature = true; // check if the streamline dlls are signed
        bool enableStreamlineLog = false;
#endif
    };

    struct DeviceCreationParameters : public InstanceParameters
    {
        bool startMaximized = false; // ignores backbuffer width/height to be monitor size
        bool startFullscreen = false;
        bool startBorderless = false;
        bool allowModeSwitch = false;
        int windowPosX = -1;            // -1 means use default placement
        int windowPosY = -1;
        uint32_t backBufferWidth = 1280;
        uint32_t backBufferHeight = 720;
        uint32_t refreshRate = 0;
        uint32_t swapChainBufferCount = 3;
        nvrhi::Format swapChainFormat = nvrhi::Format::SRGBA8_UNORM;
        uint32_t swapChainSampleCount = 1;
        uint32_t swapChainSampleQuality = 0;
        uint32_t maxFramesInFlight = 2;
        bool enableNvrhiValidationLayer = false;
        bool vsyncEnabled = false;
        bool enableRayTracingExtensions = false; // for vulkan
        bool enableComputeQueue = false;
        bool enableCopyQueue = false;

        // Index of the adapter (DX11, DX12) or physical device (Vk) on which to initialize the device.
        // Negative values mean automatic detection.
        // The order of indices matches that returned by DeviceManager::EnumerateAdapters.
        int adapterIndex = -1;

        // Set this to true if the application implements UI scaling for DPI explicitly instead of relying
        // on ImGUI's DisplayFramebufferScale. This produces crisp text and lines at any scale
        // but requires considerable changes to applications that rely on the old behavior:
        // all UI sizes and offsets need to be computed as multiples of some scaled parameter,
        // such as ImGui::GetFontSize(). Note that the ImGUI style is automatically reset and scaled in 
        // ImGui_Renderer::DisplayScaleChanged(...).
        //
        // See ImGUI FAQ for more info:
        //   https://github.com/ocornut/imgui/blob/master/docs/FAQ.md#q-how-should-i-handle-dpi-in-my-application
        bool supportExplicitDisplayScaling = false;

        // Enables automatic resizing of the application window according to the DPI scaling of the monitor
        // that it is located on. When set to true and the app launches on a monitor with >100% scale, 
        // the initial window size will be larger than specified in 'backBufferWidth' and 'backBufferHeight' parameters.
        bool resizeWindowWithDisplayScale = false;

        nvrhi::IMessageCallback *messageCallback = nullptr;

#if DONUT_WITH_DX11 || DONUT_WITH_DX12
        DXGI_USAGE swapChainUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
#endif

#if DONUT_WITH_VULKAN
        std::vector<std::string> requiredVulkanDeviceExtensions;
        std::vector<std::string> optionalVulkanDeviceExtensions;
        std::vector<size_t> ignoredVulkanValidationMessageLocations = {
            // Ignore the warnings like "the storage image descriptor [...] is accessed by a OpTypeImage that has
            //   a Format operand ... which doesn't match the VkImageView ..." -- even when the GPU supports
            // storage without format, which all modern GPUs do, there is no good way to enable it in the shaders.
            0x13365b2
        };
        std::function<void(VkDeviceCreateInfo&)> deviceCreateInfoCallback;

        // This pointer specifies an optional structure to be put at the end of the chain for 'vkGetPhysicalDeviceFeatures2' call.
        // The structure may also be a chain, and must be alive during the device initialization process.
        // The elements of this structure will be populated before 'deviceCreateInfoCallback' is called,
        // thereby allowing applications to determine if certain features may be enabled on the device.
        void* physicalDeviceFeatures2Extensions = nullptr;
#endif
    };

    class IRenderPass;

    struct AdapterInfo
    {
        typedef std::array<uint8_t, 16> UUID;
        typedef std::array<uint8_t, 8> LUID;

        std::string name;
        uint32_t vendorID = 0;
        uint32_t deviceID = 0;
        uint64_t dedicatedVideoMemory = 0;

        std::optional<UUID> uuid;
        std::optional<LUID> luid;

#if DONUT_WITH_DX11 || DONUT_WITH_DX12
        nvrhi::RefCountPtr<IDXGIAdapter> dxgiAdapter;
#endif
#if DONUT_WITH_VULKAN
        VkPhysicalDevice vkPhysicalDevice = nullptr;
#endif
    };

    class DeviceManager
    {
    public:
        static DeviceManager* Create(nvrhi::GraphicsAPI api);

        bool CreateHeadlessDevice(const DeviceCreationParameters& params);
        bool CreateWindowDeviceAndSwapChain(const DeviceCreationParameters& params, const char* windowTitle);

        // Initializes device-independent objects (DXGI factory, Vulkan instnace).
        // Calling CreateInstance() is required before EnumerateAdapters(), but optional if you don't use EnumerateAdapters().
        // Note: if you call CreateInstance before Create*Device*(), the values in InstanceParameters must match those
        // in DeviceCreationParameters passed to the device call.
        bool CreateInstance(const InstanceParameters& params);

        // Enumerates adapters or physical devices present in the system.
        // Note: a call to CreateInstance() or Create*Device*() is required before EnumerateAdapters().
        virtual bool EnumerateAdapters(std::vector<AdapterInfo>& outAdapters) = 0;

        void AddRenderPassToFront(IRenderPass *pController);
        void AddRenderPassToBack(IRenderPass *pController);
        void RemoveRenderPass(IRenderPass *pController);

        void RunMessageLoop();

        // returns the size of the window in screen coordinates
        void GetWindowDimensions(int& width, int& height);
        // returns the screen coordinate to pixel coordinate scale factor
        void GetDPIScaleInfo(float& x, float& y) const
        {
            x = m_DPIScaleFactorX;
            y = m_DPIScaleFactorY;
        }

    protected:
        // useful for apps that require 2 frames worth of simulation data before first render
        // apps should extend the DeviceManager classes, and constructor initialized this to true to opt in to the behavior
        bool m_SkipRenderOnFirstFrame = false;
        bool m_windowVisible = false;
        bool m_windowIsInFocus = true;

        DeviceCreationParameters m_DeviceParams;
        GLFWwindow *m_Window = nullptr;
        bool m_EnableRenderDuringWindowMovement = false;
        // set to true if running on NV GPU
        bool m_IsNvidia = false;
        std::list<IRenderPass *> m_vRenderPasses;
        // timestamp in seconds for the previous frame
        double m_PreviousFrameTimestamp = 0.0;
        // current DPI scale info (updated when window moves)
        float m_DPIScaleFactorX = 1.f;
        float m_DPIScaleFactorY = 1.f;
        float m_PrevDPIScaleFactorX = 0.f;
        float m_PrevDPIScaleFactorY = 0.f;
        bool m_RequestedVSync = false;
        bool m_InstanceCreated = false;
        bool m_RequestedRenderUnfocused = true;

        double m_AverageFrameTime = 0.0;
        double m_AverageTimeUpdateInterval = 0.5;
        double m_FrameTimeSum = 0.0;
        int m_NumberOfAccumulatedFrames = 0;

        uint32_t m_FrameIndex = 0;

        std::vector<nvrhi::FramebufferHandle> m_SwapChainFramebuffers;

        DeviceManager();

        void UpdateWindowSize();
        bool ShouldRenderUnfocused() const;

        void BackBufferResizing();
        void BackBufferResized();
        void DisplayScaleChanged();

        void Animate(double elapsedTime, bool windowIsFocused);
        void Render();
        void UpdateAverageFrameTime(double elapsedTime);
        bool AnimateRenderPresent();
        // device-specific methods
        virtual bool CreateInstanceInternal() = 0;
        virtual bool CreateDevice() = 0;
        virtual bool CreateSwapChain() = 0;
        virtual void DestroyDeviceAndSwapChain() = 0;
        virtual void ResizeSwapChain() = 0;
        virtual bool BeginFrame() = 0;
        virtual bool Present() = 0;

    public:
        [[nodiscard]] virtual nvrhi::IDevice *GetDevice() const = 0;
        [[nodiscard]] virtual const char *GetRendererString() const = 0;
        [[nodiscard]] virtual nvrhi::GraphicsAPI GetGraphicsAPI() const = 0;

        const DeviceCreationParameters& GetDeviceParams();
        [[nodiscard]] double GetAverageFrameTimeSeconds() const { return m_AverageFrameTime; }
        [[nodiscard]] double GetPreviousFrameTimestamp() const { return m_PreviousFrameTimestamp; }
        void SetFrameTimeUpdateInterval(double seconds) { m_AverageTimeUpdateInterval = seconds; }
        [[nodiscard]] bool IsVsyncEnabled() const { return m_DeviceParams.vsyncEnabled; }
        virtual void SetVsyncEnabled(bool enabled) { m_RequestedVSync = enabled; /* will be processed later */ }
        virtual void ReportLiveObjects() {}
        void SetEnableRenderDuringWindowMovement(bool val) {m_EnableRenderDuringWindowMovement = val;} 
        bool IsWindowFocused() const { return m_windowIsInFocus; }
        bool IsWindowVisible() const { return m_windowVisible; }

        // Call this function to make sure that the next frame is rendered even if the window is unfocused.
        // This is useful for applications that want to continue rendering while performing a long operation,
        // but otherwise do not want to render when unfocused in order to save power.
        void RenderNextFrameWhileUnfocused() { m_RequestedRenderUnfocused = true; }

        // these are public in order to be called from the GLFW callback functions
        void WindowCloseCallback() { }
        void WindowIconifyCallback(int iconified) { }
        void WindowFocusCallback(int focused) { }
        void WindowRefreshCallback() { }
        void WindowPosCallback(int xpos, int ypos);

        void KeyboardUpdate(int key, int scancode, int action, int mods);
        void KeyboardCharInput(unsigned int unicode, int mods);
        void MousePosUpdate(double xpos, double ypos);
        void MouseButtonUpdate(int button, int action, int mods);
        void MouseScrollUpdate(double xoffset, double yoffset);

        [[nodiscard]] GLFWwindow* GetWindow() const { return m_Window; }
        [[nodiscard]] uint32_t GetFrameIndex() const { return m_FrameIndex; }

        virtual nvrhi::ITexture* GetCurrentBackBuffer() = 0;
        virtual nvrhi::ITexture* GetBackBuffer(uint32_t index) = 0;
        virtual uint32_t GetCurrentBackBufferIndex() = 0;
        virtual uint32_t GetBackBufferCount() = 0;
        nvrhi::IFramebuffer* GetCurrentFramebuffer();
        nvrhi::IFramebuffer* GetFramebuffer(uint32_t index);

        virtual void Shutdown();
        virtual ~DeviceManager() = default;

        void SetWindowTitle(const char* title);
        void SetInformativeWindowTitle(const char* applicationName, bool includeFramerate = true, const char* extraInfo = nullptr);
        const char* GetWindowTitle();

        virtual bool IsVulkanInstanceExtensionEnabled(const char* extensionName) const { return false; }
        virtual bool IsVulkanDeviceExtensionEnabled(const char* extensionName) const { return false; }
        virtual bool IsVulkanLayerEnabled(const char* layerName) const { return false; }
        virtual void GetEnabledVulkanInstanceExtensions(std::vector<std::string>& extensions) const { }
        virtual void GetEnabledVulkanDeviceExtensions(std::vector<std::string>& extensions) const { }
        virtual void GetEnabledVulkanLayers(std::vector<std::string>& layers) const { }

        // GetFrameIndex cannot be used inside of these callbacks, hence the additional passing of frameID
        // Refer to AnimateRenderPresent implementation for more details
        struct PipelineCallbacks {
            std::function<void(DeviceManager&, uint32_t)> beforeFrame = nullptr;
            std::function<void(DeviceManager&, uint32_t)> beforeAnimate = nullptr;
            std::function<void(DeviceManager&, uint32_t)> afterAnimate = nullptr;
            std::function<void(DeviceManager&, uint32_t)> beforeRender = nullptr;
            std::function<void(DeviceManager&, uint32_t)> afterRender = nullptr;
            std::function<void(DeviceManager&, uint32_t)> beforePresent = nullptr;
            std::function<void(DeviceManager&, uint32_t)> afterPresent = nullptr;
        } m_callbacks;

#if DONUT_WITH_STREAMLINE
        static StreamlineInterface& GetStreamline();
#endif

    private:
        static DeviceManager* CreateD3D11();
        static DeviceManager* CreateD3D12();
        static DeviceManager* CreateVK();

        std::string m_WindowTitle;
#if DONUT_WITH_AFTERMATH
        AftermathCrashDump m_AftermathCrashDumper;
#endif
    };

    class IRenderPass
    {
    private:
        DeviceManager* m_DeviceManager;

    public:
        explicit IRenderPass(DeviceManager* deviceManager)
            : m_DeviceManager(deviceManager)
        { }

        virtual ~IRenderPass() = default;

        virtual void SetLatewarpOptions() { }
        virtual bool ShouldAnimateUnfocused() { return false; }
        virtual bool ShouldRenderUnfocused() { return false; }
        virtual void Render(nvrhi::IFramebuffer* framebuffer) { }
        virtual void Animate(float fElapsedTimeSeconds) { }
        virtual void BackBufferResizing() { }
        virtual void BackBufferResized(const uint32_t width, const uint32_t height, const uint32_t sampleCount) { }

        // Called before Animate() when a DPI change was detected
        virtual void DisplayScaleChanged(float scaleX, float scaleY) { }

        // all of these pass in GLFW constants as arguments
        // see http://www.glfw.org/docs/latest/input.html
        // return value is true if the event was consumed by this render pass, false if it should be passed on
        virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) { return false; }
        virtual bool KeyboardCharInput(unsigned int unicode, int mods) { return false; }
        virtual bool MousePosUpdate(double xpos, double ypos) { return false; }
        virtual bool MouseScrollUpdate(double xoffset, double yoffset) { return false; }
        virtual bool MouseButtonUpdate(int button, int action, int mods) { return false; }
        virtual bool JoystickButtonUpdate(int button, bool pressed) { return false; }
        virtual bool JoystickAxisUpdate(int axis, float value) { return false; }

        [[nodiscard]] DeviceManager* GetDeviceManager() const { return m_DeviceManager; }
        [[nodiscard]] nvrhi::IDevice* GetDevice() const { return m_DeviceManager->GetDevice(); }
        [[nodiscard]] uint32_t GetFrameIndex() const { return m_DeviceManager->GetFrameIndex(); }
    };
}
