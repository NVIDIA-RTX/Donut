/*
* Copyright (c) 2014-2025, NVIDIA CORPORATION. All rights reserved.
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
License for Dear ImGui

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stddef.h>

#include <imgui.h>

#include <nvrhi/nvrhi.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/app/imgui_nvrhi.h>
#include <donut/core/log.h>

#if DONUT_WITH_STATIC_SHADERS
#if DONUT_WITH_DX11
#include "compiled_shaders/imgui_vertex.dxbc.h"
#include "compiled_shaders/imgui_pixel.dxbc.h"
#endif
#if DONUT_WITH_DX12
#include "compiled_shaders/imgui_vertex.dxil.h"
#include "compiled_shaders/imgui_pixel.dxil.h"
#endif
#if DONUT_WITH_VULKAN
#include "compiled_shaders/imgui_vertex.spirv.h"
#include "compiled_shaders/imgui_pixel.spirv.h"
#endif
#endif

using namespace donut::engine;
using namespace donut::app;

struct VERTEX_CONSTANT_BUFFER
{
    float        mvp[4][4];
};

bool ImGui_NVRHI::updateFontTexture()
{
    ImGuiIO& io = ImGui::GetIO();

    // If the font texture exists and is bound to ImGui, we're done.
    // Note: ImGui_Renderer will reset io.Fonts->TexRef when new fonts are added.
    if (fontTexture && io.Fonts->TexRef.GetTexID())
        return true;

    unsigned char *pixels;
    int width, height;

    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels)
        return false;

    nvrhi::TextureDesc textureDesc;
    textureDesc.width = width;
    textureDesc.height = height;
    textureDesc.format = nvrhi::Format::RGBA8_UNORM;
    textureDesc.debugName = "ImGui font texture";

    fontTexture = m_device->createTexture(textureDesc);

    if (fontTexture == nullptr)
        return false;

    m_commandList->open();

    m_commandList->beginTrackingTextureState(fontTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);

    m_commandList->writeTexture(fontTexture, 0, 0, pixels, width * 4);

    m_commandList->setPermanentTextureState(fontTexture, nvrhi::ResourceStates::ShaderResource);
    m_commandList->commitBarriers();

    m_commandList->close();
    m_device->executeCommandList(m_commandList);

    io.Fonts->TexRef = ImTextureRef(fontTexture.Get());

    return true;
}

bool ImGui_NVRHI::init(nvrhi::IDevice* device, std::shared_ptr<ShaderFactory> shaderFactory)
{
    m_device = device;

    m_commandList = m_device->createCommandList();

    vertexShader = shaderFactory->CreateAutoShader("donut/imgui_vertex", "main", DONUT_MAKE_PLATFORM_SHADER(g_imgui_vertex), nullptr, nvrhi::ShaderType::Vertex);
    pixelShader = shaderFactory->CreateAutoShader("donut/imgui_pixel", "main", DONUT_MAKE_PLATFORM_SHADER(g_imgui_pixel), nullptr, nvrhi::ShaderType::Pixel);
    
    if (!vertexShader || !pixelShader)
    {
        log::error("Failed to create an ImGUI shader");
        return false;
    } 

    // create attribute layout object
    nvrhi::VertexAttributeDesc vertexAttribLayout[] = {
        { "POSITION", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,pos), sizeof(ImDrawVert), false },
        { "TEXCOORD", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,uv),  sizeof(ImDrawVert), false },
        { "COLOR",    nvrhi::Format::RGBA8_UNORM, 1, 0, offsetof(ImDrawVert,col), sizeof(ImDrawVert), false },
    };

    shaderAttribLayout = m_device->createInputLayout(vertexAttribLayout, sizeof(vertexAttribLayout) / sizeof(vertexAttribLayout[0]), vertexShader);

    // create PSO
    {
        nvrhi::BlendState blendState;
        blendState.targets[0].setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
            .setSrcBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
            .setDestBlendAlpha(nvrhi::BlendFactor::Zero);

        auto rasterState = nvrhi::RasterState()
            .setFillSolid()
            .setCullNone()
            .setScissorEnable(true)
            .setDepthClipEnable(true);

        auto depthStencilState = nvrhi::DepthStencilState()
            .disableDepthTest()
            .enableDepthWrite()
            .disableStencil()
            .setDepthFunc(nvrhi::ComparisonFunc::Always);

        nvrhi::RenderState renderState;
        renderState.blendState = blendState;
        renderState.depthStencilState = depthStencilState;
        renderState.rasterState = rasterState;

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::All;
        layoutDesc.bindings = { 
            nvrhi::BindingLayoutItem::PushConstants(0, sizeof(float) * 2),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Sampler(0) 
        };
        bindingLayout = m_device->createBindingLayout(layoutDesc);

        basePSODesc.primType = nvrhi::PrimitiveType::TriangleList;
        basePSODesc.inputLayout = shaderAttribLayout;
        basePSODesc.VS = vertexShader;
        basePSODesc.PS = pixelShader;
        basePSODesc.renderState = renderState;
        basePSODesc.bindingLayouts = { bindingLayout };
    }

    {
        const auto desc = nvrhi::SamplerDesc()
            .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap)
            .setAllFilters(true);

        fontSampler = m_device->createSampler(desc);

        if (fontSampler == nullptr)
            return false;
    }

    return true;
}

bool ImGui_NVRHI::reallocateBuffer(nvrhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, const bool indexBuffer)
{
    if (buffer == nullptr || size_t(buffer->getDesc().byteSize) < requiredSize)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = uint32_t(reallocateSize);
        desc.structStride = 0;
        desc.debugName = indexBuffer ? "ImGui index buffer" : "ImGui vertex buffer";
        desc.canHaveUAVs = false;
        desc.isVertexBuffer = !indexBuffer;
        desc.isIndexBuffer = indexBuffer;
        desc.isDrawIndirectArgs = false;
        desc.isVolatile = false;
        desc.initialState = indexBuffer ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::VertexBuffer;
        desc.keepInitialState = true;

        buffer = m_device->createBuffer(desc);

        if (!buffer)
        {
            return false;
        }
    }

    return true;
}

nvrhi::IGraphicsPipeline* ImGui_NVRHI::getPSO(nvrhi::FramebufferInfo const& framebufferInfo)
{
    if (pso)
        return pso;

    pso = m_device->createGraphicsPipeline(basePSODesc, framebufferInfo);
    assert(pso);

    return pso;
}

nvrhi::IBindingSet* ImGui_NVRHI::getBindingSet(nvrhi::ITexture* texture)
{
    auto iter = bindingsCache.find(texture);
    if (iter != bindingsCache.end())
    {
        return iter->second;
    }

    nvrhi::BindingSetDesc desc;

    desc.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(float) * 2),
        nvrhi::BindingSetItem::Texture_SRV(0, texture),
        nvrhi::BindingSetItem::Sampler(0, fontSampler)
    };

    nvrhi::BindingSetHandle binding;
    binding = m_device->createBindingSet(desc, bindingLayout);
    assert(binding);

    bindingsCache[texture] = binding;
    return binding;
}

bool ImGui_NVRHI::updateGeometry(nvrhi::ICommandList* commandList)
{
    ImDrawData *drawData = ImGui::GetDrawData();

    // create/resize vertex and index buffers if needed
    if (!reallocateBuffer(vertexBuffer, 
        drawData->TotalVtxCount * sizeof(ImDrawVert), 
        (drawData->TotalVtxCount + 5000) * sizeof(ImDrawVert), 
        false))
    {
        return false;
    }

    if (!reallocateBuffer(indexBuffer,
        drawData->TotalIdxCount * sizeof(ImDrawIdx),
        (drawData->TotalIdxCount + 5000) * sizeof(ImDrawIdx),
        true))
    {
        return false;
    }

    vtxBuffer.resize(vertexBuffer->getDesc().byteSize / sizeof(ImDrawVert));
    idxBuffer.resize(indexBuffer->getDesc().byteSize / sizeof(ImDrawIdx));

    // copy and convert all vertices into a single contiguous buffer
    ImDrawVert *vtxDst = &vtxBuffer[0];
    ImDrawIdx *idxDst = &idxBuffer[0];

    for(int n = 0; n < drawData->CmdListsCount; n++)
    {
        const ImDrawList *cmdList = drawData->CmdLists[n];

        memcpy(vtxDst, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(idxDst, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));

        vtxDst += cmdList->VtxBuffer.Size;
        idxDst += cmdList->IdxBuffer.Size;
    }
    
    commandList->writeBuffer(vertexBuffer, &vtxBuffer[0], vertexBuffer->getDesc().byteSize);
    commandList->writeBuffer(indexBuffer, &idxBuffer[0], indexBuffer->getDesc().byteSize);

    return true;
}

bool ImGui_NVRHI::render(nvrhi::IFramebuffer* framebuffer)
{
    ImDrawData *drawData = ImGui::GetDrawData();
    const auto& io = ImGui::GetIO();

    m_commandList->open();
    m_commandList->beginMarker("ImGUI");

    if (!updateGeometry(m_commandList))
    {
        m_commandList->close();
        return false;
    }

    // handle DPI scaling
    drawData->ScaleClipRects(io.DisplayFramebufferScale);

    float invDisplaySize[2] = { 1.f / io.DisplaySize.x, 1.f / io.DisplaySize.y };

    // set up graphics state
    nvrhi::GraphicsState drawState;

    drawState.framebuffer = framebuffer;
    assert(drawState.framebuffer);
    
    drawState.pipeline = getPSO(framebuffer->getFramebufferInfo());

    drawState.viewport.viewports.push_back(nvrhi::Viewport(io.DisplaySize.x * io.DisplayFramebufferScale.x,
                                           io.DisplaySize.y * io.DisplayFramebufferScale.y));
    drawState.viewport.scissorRects.resize(1);  // updated below

    nvrhi::VertexBufferBinding vbufBinding;
    vbufBinding.buffer = vertexBuffer;
    vbufBinding.slot = 0;
    vbufBinding.offset = 0;
    drawState.vertexBuffers.push_back(vbufBinding);

    drawState.indexBuffer.buffer = indexBuffer;
    drawState.indexBuffer.format = (sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT);
    drawState.indexBuffer.offset = 0;

    // render command lists
    int vtxOffset = 0;
    int idxOffset = 0;
    for(int n = 0; n < drawData->CmdListsCount; n++)
    {
        const ImDrawList *cmdList = drawData->CmdLists[n];
        for(int i = 0; i < cmdList->CmdBuffer.Size; i++)
        {
            const ImDrawCmd *pCmd = &cmdList->CmdBuffer[i];

            if (pCmd->UserCallback)
            {
                pCmd->UserCallback(cmdList, pCmd);
            } else {
                // Skip draw commands with a degenerate (zero-area) integer clip rect;
                // they set an empty scissor with a non-empty viewport (debug-layer warning).
                if (int(pCmd->ClipRect.z) > int(pCmd->ClipRect.x) &&
                    int(pCmd->ClipRect.w) > int(pCmd->ClipRect.y))
                {
                    drawState.bindings = { getBindingSet((nvrhi::ITexture*)pCmd->TexRef.GetTexID()) };
                    assert(drawState.bindings[0]);

                    drawState.viewport.scissorRects[0] = nvrhi::Rect(int(pCmd->ClipRect.x),
                                                                     int(pCmd->ClipRect.z),
                                                                     int(pCmd->ClipRect.y),
                                                                     int(pCmd->ClipRect.w));

                    nvrhi::DrawArguments drawArguments;
                    drawArguments.vertexCount = pCmd->ElemCount;
                    drawArguments.startIndexLocation = idxOffset;
                    drawArguments.startVertexLocation = vtxOffset;

                    m_commandList->setGraphicsState(drawState);
                    m_commandList->setPushConstants(invDisplaySize, sizeof(invDisplaySize));
                    m_commandList->drawIndexed(drawArguments);
                }
            }

            idxOffset += pCmd->ElemCount;
        }

        vtxOffset += cmdList->VtxBuffer.Size;
    }

    m_commandList->endMarker();
    m_commandList->close();
    m_device->executeCommandList(m_commandList);

    return true;
}

void ImGui_NVRHI::backbufferResizing()
{
    pso = nullptr;
}
