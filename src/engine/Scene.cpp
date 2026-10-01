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

/*
License for JsonCpp

JsonCpp is Public Domain

The JsonCpp library's source code, including accompanying documentation, 
tests and demonstration applications, are licensed under the following
conditions...

Baptiste Lepilleur and The JsonCpp Authors explicitly disclaim copyright in all 
jurisdictions which recognize such a disclaimer. In such jurisdictions, 
this software is released into the Public Domain.
*/

#include <donut/engine/Scene.h>
#include <donut/engine/GltfImporter.h>
#include <donut/engine/ThreadPool.h>
#include <donut/core/json.h>
#include <donut/core/log.h>
#include <donut/core/math/float.h>
#include <donut/core/string_utils.h>
#include <nvrhi/common/misc.h>
#include <json/json-forwards.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "donut/engine/ShaderFactory.h"

#if DONUT_WITH_STATIC_SHADERS
#if DONUT_WITH_DX11
#include "compiled_shaders/skinning_cs.dxbc.h"
#endif
#if DONUT_WITH_DX12
#include "compiled_shaders/skinning_cs.dxil.h"
#endif
#if DONUT_WITH_VULKAN
#include "compiled_shaders/skinning_cs.spirv.h"
#endif
#endif

using namespace donut::math;
#include <donut/shaders/material_cb.h>
#include <donut/shaders/skinning_cb.h>
#include <donut/shaders/bindless.h>

using namespace donut::vfs;
using namespace donut::engine;

static SceneLoadingStats g_LoadingStats;

const SceneLoadingStats& Scene::GetLoadingStats()
{
    return g_LoadingStats;
}

struct Scene::Resources
{
    std::vector<MaterialConstants> materialData;
    std::vector<GeometryData> geometryData;
    std::vector<InstanceData> instanceData;
};

Scene::Scene(
    nvrhi::IDevice* device,
    ShaderFactory& shaderFactory,
    std::shared_ptr<IFileSystem> fs,
    std::shared_ptr<TextureCache> textureCache,
    std::shared_ptr<DescriptorTableManager> descriptorTable,
    std::shared_ptr<SceneTypeFactory> sceneTypeFactory)
    : m_fs(std::move(fs))
    , m_SceneTypeFactory(std::move(sceneTypeFactory))
    , m_TextureCache(std::move(textureCache))
    , m_DescriptorTable(std::move(descriptorTable))
    , m_Device(device)
{
    m_Resources = std::make_shared<Resources>();

    if (!m_SceneTypeFactory)
        m_SceneTypeFactory = std::make_shared<SceneTypeFactory>();

    m_GltfImporter = std::make_shared<GltfImporter>(m_fs, m_SceneTypeFactory);

    m_EnableBindlessResources = !!m_DescriptorTable;
    m_RayTracingSupported = m_Device->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct);

    m_SkinningShader = shaderFactory.CreateAutoShader("donut/skinning_cs", "main", DONUT_MAKE_PLATFORM_SHADER(g_skinning_cs), nullptr, nvrhi::ShaderType::Compute);

    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SkinningConstants)),
            nvrhi::BindingLayoutItem::RawBuffer_SRV(0),
            nvrhi::BindingLayoutItem::RawBuffer_SRV(1),
            nvrhi::BindingLayoutItem::RawBuffer_UAV(0)
        };

        m_SkinningBindingLayout = m_Device->createBindingLayout(layoutDesc);
    }

    {
        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.bindingLayouts = { m_SkinningBindingLayout };
        pipelineDesc.CS = m_SkinningShader;
        m_SkinningPipeline = m_Device->createComputePipeline(pipelineDesc);
    }
}

void Scene::SetDefaultTexCoordFormat(TexCoordFormat format)
{
    m_GltfImporter = std::make_shared<GltfImporter>(m_fs, m_SceneTypeFactory, format);
}

bool Scene::Load(const std::filesystem::path& jsonFileName)
{
    ThreadPool threadPool;
    return LoadWithThreadPool(jsonFileName, &threadPool);
}

bool Scene::LoadWithThreadPool(const std::filesystem::path& sceneFileName, ThreadPool* threadPool)
{
    g_LoadingStats.ObjectsLoaded = 0;
    g_LoadingStats.ObjectsTotal = 0;
    
    m_SceneGraph = m_SceneTypeFactory->CreateGraph();

    if (sceneFileName.extension() == ".gltf" || sceneFileName.extension() == ".glb")
    {
        ++g_LoadingStats.ObjectsTotal;
        m_Models.resize(1);
        LoadModelAsync(0, sceneFileName, threadPool);

        if (threadPool)
            threadPool->WaitForTasks();

        auto modelResult = m_Models[0];
        if (!modelResult.rootNode)
            return false;

        m_SceneGraph->SetRootNode(modelResult.rootNode);
    }
    else
    {
        std::shared_ptr<SceneGraphNode> rootNode = std::make_shared<SceneGraphNode>();
        rootNode->SetName("SceneRoot");
        m_SceneGraph->SetRootNode(rootNode);

        std::filesystem::path scenePath = sceneFileName.parent_path();

        Json::Value documentRoot;
        if (!json::LoadFromFile(*m_fs, sceneFileName, documentRoot))
            return false;

        if (documentRoot.isObject())
        {
            if (!LoadCustomData(documentRoot, scenePath, threadPool))
                return false;

            LoadModels(documentRoot["models"], scenePath, threadPool);
            LoadSceneGraph(documentRoot["graph"], rootNode);
            LoadAnimations(documentRoot["animations"]);
        }
        else
        {
            log::error("Unrecognized structure of the scene description file.");
            return false;
        }
    }

    return true;
}

void Scene::LoadModelAsync(
    uint32_t index,
    const std::filesystem::path& fileName,
    ThreadPool* threadPool)
{   
    // Retain the import settings for this model even if a later load changes the default.
    const auto importer = m_GltfImporter;
    if (threadPool)
    {
        threadPool->AddTask([this, index, threadPool, fileName, importer]()
        {
            SceneImportResult result;
            importer->Load(fileName, *m_TextureCache, g_LoadingStats, threadPool, result);
            ++g_LoadingStats.ObjectsLoaded;
            m_Models[index] = result;
        });
    }
    else
    {
        SceneImportResult result;
        importer->Load(fileName, *m_TextureCache, g_LoadingStats, threadPool, result);
        ++g_LoadingStats.ObjectsLoaded;
        m_Models[index] = result;
    }
}

void Scene::LoadModels(
    const Json::Value& modelList,
    const std::filesystem::path& scenePath,
    ThreadPool* threadPool)
{
    if (!modelList.isArray())
    {
        return;
    }

    m_Models.resize(modelList.size());
    uint32_t index = 0;
    for (const auto& model : modelList)
    {
        ++g_LoadingStats.ObjectsTotal;

        std::filesystem::path fileName = scenePath / std::filesystem::path(model.asString());

        LoadModelAsync(index, fileName, threadPool);

        ++index;
    }

    if (threadPool)
        threadPool->WaitForTasks();
}

void Scene::LoadSceneGraph(const Json::Value& nodeList, const std::shared_ptr<SceneGraphNode>& parent)
{
    for (const auto& src : nodeList)
    {
        if (!src.isObject())
        {
            log::warning("Non-object node in the scene graph definition.");
            continue;
        }

        std::string nodeName;
        const auto& name = src["name"];
        if (name.isString())
        {
            nodeName = name.asString();
        }

        std::shared_ptr<SceneGraphNode> customParent = parent;
        const auto& parentNode = src["parent"];
        if (parentNode.isString())
        {
            customParent = m_SceneGraph->FindNode(parentNode.asString());
            if (!customParent)
            {
                log::warning("Custom parent '%s' specified for node '%s' not found, skipping the node.",
                    parentNode.asCString(), nodeName.c_str());
                continue;
            }
        }
        else if (!parentNode.isNull())
        {
            log::warning("Custom parent specification for node '%s' is not a string, ignoring.",
                nodeName.c_str());
        }

        std::shared_ptr<SceneGraphNode> dst;

        const auto& modelNode = src["model"];
        if (!modelNode.isNull())
        {
            if (!modelNode.isIntegral())
            {
                log::warning("Model references in the scene graph must be indices into the model array.");
                continue;
            }

            int modelIndex = modelNode.asInt();
            if (modelIndex < 0 || modelIndex >= int(m_Models.size()))
            {
                log::warning("Referenced model %d is not defined in the model array.", modelIndex);
                continue;
            }

            const auto& loadedModel = m_Models[modelIndex];
            if (!loadedModel.rootNode)
            {
                continue;
            }

            dst = loadedModel.rootNode;
        }
        else
        {
            dst = std::make_shared<SceneGraphNode>();
        }

        dst = m_SceneGraph->Attach(customParent, dst);

        dst->SetName(nodeName);
        
        const auto& translation = src["translation"];
        if (!translation.isNull())
        {
            double3 value = double3::zero();
            translation >> value;
            dst->SetTranslation(value);
        }

        const auto& rotation = src["rotation"];
        if (!rotation.isNull())
        {
            double4 value = double4(0.0, 0.0, 0.0, 1.0);
            rotation >> value;
            dst->SetRotation(dm::dquat::fromXYZW(value));
        }
        else
        {
            const auto& euler = src["euler"];
            if (!euler.isNull())
            {
                double3 value = double3::zero();
                euler >> value;
                dst->SetRotation(rotationQuat(value));
            }
        }

        const auto& scaling = src["scaling"];
        if (!scaling.isNull())
        {
            double3 value = double3(1.0);
            scaling >> value;
            dst->SetScaling(value);
        }

        const auto& children = src["children"];
        if (!children.isNull())
        {
            LoadSceneGraph(children, dst);
        }

        const auto& leafTypeNode = src["type"];
        if (leafTypeNode.isString())
        {
            auto leaf = m_SceneTypeFactory->CreateLeaf(leafTypeNode.asString());
            if (leaf)
            {
                dst->SetLeaf(leaf);
                leaf->Load(src);
            }
            else
            {
                log::warning("Unknown leaf type '%s' for node '%s', skipping.",
                    leafTypeNode.asCString(), dst->GetName().c_str());
            }
        }
        else if (!leafTypeNode.isNull())
        {
            log::warning("Leaf type specification for node '%s' is not a string, skipping.",
                dst->GetName().c_str());
        }
    }
}

static dm::float4 ReadUpToFloat4(const Json::Value& node)
{
    if (node.isNumeric())
        return dm::float4(node.asFloat());

    if (node.isArray())
    {
        float4 result = float4::zero();
        for (int i = 0; i < std::min(4, int(node.size())); i++)
        {
            result[i] = node[i].asFloat();
        }
        return result;
    }

    return float4::zero();
}

void Scene::LoadAnimations(const Json::Value& nodeList)
{
    std::shared_ptr<SceneGraphNode> animationContainer;

    for (const auto& animationNode : nodeList)
    {
        const auto& animation = std::make_shared<SceneGraphAnimation>();

        const auto& sceneAnimationNode = std::make_shared<SceneGraphNode>();
        sceneAnimationNode->SetLeaf(animation);

        const auto& nameNode = animationNode["name"];
        if (nameNode.isString())
        {
            animation->SetName(nameNode.asString());
        }

        const auto& channelsNode = animationNode["channels"];
        if (channelsNode.isArray())
        {
            int channelIndex = -1;
            for (const auto& channelSrc : channelsNode)
            {
                // Increment the index in the beginning because there are 'continue' statements below
                ++channelIndex;

                const auto& sampler = std::make_shared<animation::Sampler>();

                const auto& modeNode = channelSrc["mode"];
                if (modeNode.isString())
                {
                    if (modeNode.asString() == "step")
                        sampler->SetInterpolationMode(animation::InterpolationMode::Step);
                    else if (modeNode.asString() == "linear")
                        sampler->SetInterpolationMode(animation::InterpolationMode::Linear);
                    else if (modeNode.asString() == "slerp")
                        sampler->SetInterpolationMode(animation::InterpolationMode::Slerp);
                    else if (modeNode.asString() == "hermite")
                        sampler->SetInterpolationMode(animation::InterpolationMode::HermiteSpline);
                    else if (modeNode.asString() == "catmull-rom")
                        sampler->SetInterpolationMode(animation::InterpolationMode::CatmullRomSpline);
                    else
                        log::warning("Unknown interpolation mode '%s' specified for animation '%s' channel %d. "
                            "Valid interpolation modes are: step, linear, hermite, catmull-rom.",
                            modeNode.asCString(), animation->GetName().c_str(), channelIndex);
                }
                else
                {
                    sampler->SetInterpolationMode(animation::InterpolationMode::Step);
                    log::warning("Interpolation mode is not specified for animation '%s' channel %d, using step.",
                        animation->GetName().c_str(), channelIndex);
                }

                const auto& attributeNode = channelSrc["attribute"];
                AnimationAttribute attribute = AnimationAttribute::Undefined;
                if (attributeNode.isString() && !attributeNode.asString().empty())
                {
                    if (attributeNode.asString() == "translation")
                        attribute = AnimationAttribute::Translation;
                    else if (attributeNode.asString() == "rotation")
                        attribute = AnimationAttribute::Rotation;
                    else if (attributeNode.asString() == "scaling")
                        attribute = AnimationAttribute::Scaling;
                    else
                        attribute = AnimationAttribute::LeafProperty;
                }
                else
                {
                    log::warning("Attribute is not specified for animation '%s' channel %d, ignoring.",
                        animation->GetName().c_str(), channelIndex);
                    continue;
                }

                int keyframeIndex = -1;
                for (const auto& dataPoint : channelSrc["data"])
                {
                    ++keyframeIndex;

                    const auto& timeNode = dataPoint["time"];
                    if (!timeNode.isNumeric())
                    {
                        log::warning("Invalid keyframe %d in animation '%s' channel %d: time is not specified or is not numeric.",
                            keyframeIndex, animation->GetName().c_str(), channelIndex);
                        continue;
                    }

                    animation::Keyframe keyframe;
                    keyframe.time = timeNode.asFloat();
                    keyframe.value = ReadUpToFloat4(dataPoint["value"]);
                    keyframe.inTangent = ReadUpToFloat4(dataPoint["inTangent"]);
                    keyframe.outTangent = ReadUpToFloat4(dataPoint["outTangent"]);

                    sampler->AddKeyframe(keyframe);
                }

                auto processTargetNode = [this, &animation, &sampler, attribute, &attributeNode, channelIndex](const Json::Value& targetNode)
                {
                    if (targetNode.isString())
                    {
                        std::string targetName = targetNode.asString();
                        if (donut::string_utils::starts_with(targetName, "material:"))
                        {
                            targetName = targetName.substr(9);

                            std::shared_ptr<Material> material;
                            for (const auto& it : m_SceneGraph->GetMaterials())
                            {
                                if (it->name == targetName)
                                {
                                    material = it;
                                    break;
                                }
                            }

                            if (material)
                            {
                                const auto& channel = std::make_shared<SceneGraphAnimationChannel>(sampler, material);
                                channel->SetLeafProperyName(attributeNode.asString());
                                animation->AddChannel(channel);
                            }
                            else
                            {
                                log::warning("Target material '%s' specified for animation '%s' channel %d not found, ignoring.",
                                    std::string(targetName).c_str(), animation->GetName().c_str(), channelIndex);
                            }
                        }
                        else
                        {
                            const auto& target = m_SceneGraph->FindNode(targetNode.asString());
                            if (target)
                            {
                                const auto& channel = std::make_shared<SceneGraphAnimationChannel>(sampler, target, attribute);
                                if (attribute == AnimationAttribute::LeafProperty)
                                    channel->SetLeafProperyName(attributeNode.asString());
                                animation->AddChannel(channel);
                            }
                            else
                            {
                                log::warning("Target node '%s' specified for animation '%s' channel %d not found, ignoring.",
                                    targetNode.asCString(), animation->GetName().c_str(), channelIndex);
                            }
                        }
                    }
                    else if (!targetNode.isNull())
                    {
                        log::warning("Target node specification for animation '%s' channel %d is not a string, ignoring.",
                            animation->GetName().c_str(), channelIndex);
                    }
                };

                const auto& targetNode = channelSrc["target"];
                if (!targetNode.isNull())
                {
                    processTargetNode(targetNode);
                }
                else
                {
                    const auto& targetsNode = channelSrc["targets"];
                    if (targetsNode.isArray())
                    {
                        for (const auto& targetArrayItem : targetsNode)
                        {
                            processTargetNode(targetArrayItem);
                        }
                    }
                }
            }
        }

        if (!animation->GetChannels().empty())
        {
            if (!animationContainer)
            {
                animationContainer = std::make_shared<SceneGraphNode>();
                animationContainer->SetName("Animations");
                m_SceneGraph->Attach(m_SceneGraph->GetRootNode(), animationContainer);
            }
            
            m_SceneGraph->Attach(animationContainer, sceneAnimationNode);
        }
        else
        {
            log::warning("Animation '%s' processed with no valid channels, ignoring.",
                animation->GetName().c_str());
        }
    }
}

bool Scene::LoadCustomData(Json::Value& rootNode, const std::filesystem::path& scenePath, ThreadPool* threadPool)
{
    // Reserved for derived classes
    return true;
}

std::shared_ptr<SceneGraph> Scene::CreateSceneGraph()
{
    return m_SceneGraph = m_SceneTypeFactory->CreateGraph();
}

void Scene::FinishedLoading(uint32_t frameIndex)
{
    nvrhi::CommandListHandle commandList = m_Device->createCommandList();
    commandList->open();
    
    CreateMeshBuffers(commandList);
    Refresh(commandList, frameIndex);

    commandList->close();
    m_Device->executeCommandList(commandList);
}

void Scene::RefreshSceneGraph(uint32_t frameIndex)
{
    m_SceneStructureChanged = m_SceneGraph->HasPendingStructureChanges();
    m_SceneTransformsChanged = m_SceneGraph->HasPendingTransformChanges();
    m_SceneGraph->Refresh(frameIndex);
}

void Scene::RefreshBuffers(nvrhi::ICommandList* commandList, uint32_t frameIndex)
{
    bool materialsChanged = false;

    if (m_SceneStructureChanged)
        CreateMeshBuffers(commandList);

    const size_t allocationGranularity = 1024;
    bool arraysAllocated = false;

    if (m_EnableBindlessResources && m_SceneGraph->GetGeometryCount() > m_Resources->geometryData.size())
    {
        m_Resources->geometryData.resize(nvrhi::align<size_t>(m_SceneGraph->GetGeometryCount(), allocationGranularity));
        m_GeometryBuffer = CreateGeometryBuffer();
        arraysAllocated = true;
    }

    if (m_SceneGraph->GetMaterials().size() > m_Resources->materialData.size())
    {
        m_Resources->materialData.resize(nvrhi::align<size_t>(m_SceneGraph->GetMaterials().size(), allocationGranularity));
        if (m_EnableBindlessResources)
            m_MaterialBuffer = CreateMaterialBuffer();
        arraysAllocated = true;
    }

    if (m_SceneGraph->GetMeshInstances().size() > m_Resources->instanceData.size())
    {
        m_Resources->instanceData.resize(nvrhi::align<size_t>(m_SceneGraph->GetMeshInstances().size(), allocationGranularity));
        m_InstanceBuffer = CreateInstanceBuffer();
        arraysAllocated = true;
    }

    for (const auto& material : m_SceneGraph->GetMaterials())
    {
        if (material->dirty || m_SceneStructureChanged || arraysAllocated)
            UpdateMaterial(material);

        if (!material->materialConstants)
        {
            material->materialConstants = CreateMaterialConstantBuffer(material->name);
            material->dirty = true;
        }

        if (material->dirty)
        {
            commandList->writeBuffer(material->materialConstants,
                &m_Resources->materialData[material->materialID],
                sizeof(MaterialConstants));

            material->dirty = false;
            materialsChanged = true;
        }
    }

    if (!m_Resources->geometryData.empty())
    {
        uint32_t geometryResourceIndex = 0;
        for (const auto& mesh : m_SceneGraph->GetMeshes())
        {
            if (arraysAllocated)
            {
                break;
            }

            for (const auto& geometry : mesh->geometries)
            {
                if (geometry->numIndices != m_Resources->geometryData[geometryResourceIndex].numIndices)
                {
                    arraysAllocated = true;
                    break;
                }
                ++geometryResourceIndex;
            }
        }
    }

    if (m_SceneStructureChanged || arraysAllocated)
    {
        for (const auto& mesh : m_SceneGraph->GetMeshes())
        {
            mesh->buffers->instanceBuffer = m_InstanceBuffer;

            if (m_EnableBindlessResources)
                UpdateGeometry(mesh);
        }

        if (m_EnableBindlessResources)
            WriteGeometryBuffer(commandList);
    }

    if (m_SceneStructureChanged || m_SceneTransformsChanged || arraysAllocated)
    {
        for (const auto& instance : m_SceneGraph->GetMeshInstances())
        {
            UpdateInstance(instance);
        }

        WriteInstanceBuffer(commandList);
    }

    if (m_EnableBindlessResources && (materialsChanged || m_SceneStructureChanged || arraysAllocated))
    {
        WriteMaterialBuffer(commandList);
    }

    UpdateSkinnedMeshes(commandList, frameIndex);
}

void Scene::UpdateSkinnedMeshes(nvrhi::ICommandList* commandList, uint32_t frameIndex)
{
    bool skinningMarkerPlaced = false;

    std::vector<dm::float4x4> jointMatrices;
    for (const auto& skinnedInstance : m_SceneGraph->GetSkinnedMeshInstances())
    {
        // Only process the groups that were updated on this or previous frame.
        // Previous frame updates should be processed to copy the current positions to the previous buffer.
        if (skinnedInstance->GetLastUpdateFrameIndex() + 1 < frameIndex)
            continue;

        if (!skinningMarkerPlaced)
        {
            commandList->beginMarker("Skinning");
            skinningMarkerPlaced = true;
        }

        const auto& groupName = skinnedInstance->GetName();
        if (!groupName.empty())
            commandList->beginMarker(groupName.c_str());

        jointMatrices.resize(skinnedInstance->joints.size());
        dm::daffine3 worldToRoot = inverse(skinnedInstance->GetNode()->GetLocalToWorldTransform());

        for (size_t i = 0; i < skinnedInstance->joints.size(); i++)
        {
            auto jointNode = skinnedInstance->joints[i].node.lock();

            dm::float4x4 jointMatrix = dm::affineToHomogeneous(dm::affine3(jointNode->GetLocalToWorldTransform() * worldToRoot));
            jointMatrix = skinnedInstance->joints[i].inverseBindMatrix * jointMatrix;
            jointMatrices[i] = jointMatrix;
        }

        commandList->writeBuffer(skinnedInstance->jointBuffer, jointMatrices.data(), jointMatrices.size() * sizeof(float4x4));

        nvrhi::ComputeState state;
        state.pipeline = m_SkinningPipeline;
        state.bindings = { skinnedInstance->skinningBindingSet };
        commandList->setComputeState(state);

        uint32_t vertexOffset = skinnedInstance->GetPrototypeMesh()->vertexOffset;
        const auto& prototypeBuffers = skinnedInstance->GetPrototypeMesh()->buffers;
        const auto& skinnedBuffers = skinnedInstance->GetMesh()->buffers;

        SkinningConstants constants{};
        constants.numVertices = skinnedInstance->GetPrototypeMesh()->totalVertices;

        constants.flags = 0;
        if (prototypeBuffers->hasAttribute(VertexAttribute::Normal)) constants.flags |= SkinningFlag_Normals;
        if (prototypeBuffers->hasAttribute(VertexAttribute::Tangent)) constants.flags |= SkinningFlag_Tangents;
        if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord1)) constants.flags |= SkinningFlag_TexCoord1;
        if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord2)) constants.flags |= SkinningFlag_TexCoord2;
        if (prototypeBuffers->getTexCoordStride() == sizeof(uint32_t)) constants.flags |= SkinningFlag_TexCoords16Bit;
        if (!skinnedInstance->skinningInitialized) constants.flags |= SkinningFlag_FirstFrame;
        skinnedInstance->skinningInitialized = true;

        constants.inputPositionOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::Position).byteOffset + vertexOffset * sizeof(float3));
        constants.inputNormalOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset + vertexOffset * sizeof(uint32_t));
        constants.inputTangentOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset + vertexOffset * sizeof(uint32_t));
        constants.inputTexCoord1Offset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset + vertexOffset * prototypeBuffers->getTexCoordStride());
        constants.inputTexCoord2Offset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::TexCoord2).byteOffset + vertexOffset * prototypeBuffers->getTexCoordStride());
        constants.inputJointIndexOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::JointIndices).byteOffset + vertexOffset * sizeof(uint2));
        constants.inputJointWeightOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::JointWeights).byteOffset + vertexOffset * sizeof(float4));
        constants.outputPositionOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::Position).byteOffset);
        constants.outputPrevPositionOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset);
        constants.outputNormalOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset);
        constants.outputTangentOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset);
        constants.outputTexCoord1Offset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        constants.outputTexCoord2Offset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord2).byteOffset);
        commandList->setPushConstants(&constants, sizeof(constants));

        commandList->dispatch(dm::div_ceil(constants.numVertices, 256));

        if (!groupName.empty())
            commandList->endMarker();
    }

    if (skinningMarkerPlaced)
    {
        commandList->endMarker();
    }
}

void Scene::Refresh(nvrhi::ICommandList* commandList, uint32_t frameIndex)
{
    RefreshSceneGraph(frameIndex);
    RefreshBuffers(commandList, frameIndex);
}


nvrhi::BufferHandle CreateMaterialConstantBuffer(nvrhi::IDevice* device, const std::string& debugName, bool isVirtual)
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants);
    bufferDesc.debugName = debugName;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;
    bufferDesc.isVirtual = isVirtual;

    return device->createBuffer(bufferDesc);
}


inline void AppendBufferRange(nvrhi::BufferRange& range, size_t size, uint64_t& currentBufferSize)
{
    range.byteOffset = currentBufferSize;
    range.byteSize = nvrhi::align(size, size_t(16));
    currentBufferSize += range.byteSize;
}

static bool CanUseFloat16TexCoords(const std::vector<float2>& texcoords)
{
    for (const float2& uv : texcoords)
    {
        if (!std::isfinite(uv.x) || !std::isfinite(uv.y) || std::abs(uv.x) > 65504.f || std::abs(uv.y) > 65504.f)
            return false;
    }

    return true;
}

static bool ComputeTexCoordDecode(const std::vector<float2>& texcoords, const TexCoordDecodeRange& range,
    TexCoordDecode& decode)
{
    const size_t first = range.vertexOffset;
    const size_t end = std::min(texcoords.size(), size_t(range.vertexOffset) + range.numVertices);
    if (first >= end)
        return true;

    float2 minimum = texcoords[first];
    float2 maximum = minimum;
    for (size_t vertex = first; vertex < end; ++vertex)
    {
        const float2 uv = texcoords[vertex];
        if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
            return false;
        minimum = dm::min(minimum, uv);
        maximum = dm::max(maximum, uv);
    }

    for (int axis = 0; axis < 2; ++axis)
    {
        // Subtract in double: finite FP32 endpoints can have a range that overflows FP32.
        const double span = double(maximum[axis]) - double(minimum[axis]);
        if (span > double(std::numeric_limits<float>::max()))
            return false;

        decode.scale[axis] = float(span);
        decode.offset[axis] = minimum[axis];
        // Validate the actual stored scale as its rounding can overflow an otherwise finite endpoint.
        const double decodedMaximum = double(decode.scale[axis]) + double(decode.offset[axis]);
        if (decodedMaximum > double(std::numeric_limits<float>::max()))
            return false;
    }

    return true;
}

static bool PrepareUnorm16TexCoords(BufferGroup& buffers, std::vector<TexCoordDecodeRange> meshRanges)
{
    buffers.texCoordDecodeRanges.clear();
    const size_t vertexCount = std::max(buffers.texcoord1Data.size(), buffers.texcoord2Data.size());
    if (vertexCount == 0)
        return true;
    if (vertexCount > std::numeric_limits<uint32_t>::max())
        return false;

    std::sort(meshRanges.begin(), meshRanges.end(), [](const TexCoordDecodeRange& a, const TexCoordDecodeRange& b)
        { return a.vertexOffset < b.vertexOffset; });

    std::vector<TexCoordDecodeRange> mergedRanges;
    for (auto range : meshRanges)
    {
        if (range.vertexOffset >= vertexCount || range.numVertices == 0)
            continue;
        const uint32_t end = uint32_t(std::min(uint64_t(vertexCount), uint64_t(range.vertexOffset) + range.numVertices));
        range.numVertices = end - range.vertexOffset;
        if (!mergedRanges.empty()
            && range.vertexOffset < mergedRanges.back().vertexOffset + mergedRanges.back().numVertices)
        {
            // One packed vertex cannot use different decode transforms in overlapping meshes.
            auto& previous = mergedRanges.back();
            previous.numVertices = std::max(previous.vertexOffset + previous.numVertices, end) - previous.vertexOffset;
        }
        else
            mergedRanges.push_back(range);
    }

    uint32_t nextVertex = 0;
    for (const auto& range : mergedRanges)
    {
        if (range.vertexOffset > nextVertex)
            buffers.texCoordDecodeRanges.push_back({ nextVertex, range.vertexOffset - nextVertex });
        buffers.texCoordDecodeRanges.push_back(range);
        nextVertex = range.vertexOffset + range.numVertices;
    }
    if (nextVertex < vertexCount)
        buffers.texCoordDecodeRanges.push_back({ nextVertex, uint32_t(vertexCount) - nextVertex });

    for (auto& range : buffers.texCoordDecodeRanges)
    {
        if (!ComputeTexCoordDecode(buffers.texcoord1Data, range, range.texCoord1)
            || !ComputeTexCoordDecode(buffers.texcoord2Data, range, range.texCoord2))
        {
            buffers.texCoordDecodeRanges.clear();
            return false;
        }
    }

    return true;
}

static uint16_t PackUnorm16TexCoord(float value, float scale, float offset)
{
    if (scale == 0.f)
        return 0;
    // Avoid FP32 intermediate rounding that can cross a UNORM16 quantization midpoint.
    const double normalized = (double(value) - double(offset)) / double(scale);
    return uint16_t(std::clamp(normalized, 0.0, 1.0) * 65535.0 + 0.5);
}

static void WriteTexCoords(nvrhi::ICommandList* commandList, const BufferGroup& buffers,
    VertexAttribute attribute, std::vector<float2>& texcoords)
{
    if (texcoords.empty())
        return;

    const auto& range = buffers.getVertexBufferRange(attribute);
    if (buffers.texCoordFormat == TexCoordFormat::Float16)
    {
        std::vector<float16_t2> packed(texcoords.size());
        for (size_t index = 0; index < texcoords.size(); ++index)
            packed[index] = Float32ToFloat16x2(texcoords[index]);

        commandList->writeBuffer(buffers.vertexBuffer, packed.data(), packed.size() * sizeof(float16_t2), range.byteOffset);
    }
    else if (buffers.texCoordFormat == TexCoordFormat::Unorm16)
    {
        std::vector<uint32_t> packed(texcoords.size());
        for (const auto& decodeRange : buffers.texCoordDecodeRanges)
        {
            const auto& decode = attribute == VertexAttribute::TexCoord1 ? decodeRange.texCoord1 : decodeRange.texCoord2;
            const size_t end = std::min(texcoords.size(), size_t(decodeRange.vertexOffset) + decodeRange.numVertices);
            for (size_t vertex = decodeRange.vertexOffset; vertex < end; ++vertex)
            {
                const uint32_t u = PackUnorm16TexCoord(texcoords[vertex].x, decode.scale.x, decode.offset.x);
                const uint32_t v = PackUnorm16TexCoord(texcoords[vertex].y, decode.scale.y, decode.offset.y);
                packed[vertex] = u | (v << 16);
            }
        }
        commandList->writeBuffer(buffers.vertexBuffer, packed.data(), packed.size() * sizeof(uint32_t), range.byteOffset);
    }
    else
    {
        commandList->writeBuffer(buffers.vertexBuffer, texcoords.data(), texcoords.size() * sizeof(float2), range.byteOffset);
    }

    std::vector<float2>().swap(texcoords);
}

void Scene::CreateMeshBuffers(nvrhi::ICommandList* commandList)
{
    // Gather all meshes before uploading a shared buffer so iteration order cannot affect its encoding.
    std::unordered_map<BufferGroup*, std::vector<TexCoordDecodeRange>> texCoordMeshRanges;
    for (const auto& mesh : m_SceneGraph->GetMeshes())
    {
        if (!mesh->buffers || mesh->buffers->vertexBuffer || mesh->buffers->texCoordFormat != TexCoordFormat::Unorm16)
            continue;

        uint64_t numVertices = mesh->totalVertices;
        for (const auto& geometry : mesh->geometries)
            numVertices = std::max(numVertices, uint64_t(geometry->vertexOffsetInMesh) + geometry->numVertices);
        numVertices = std::min(numVertices, uint64_t(std::numeric_limits<uint32_t>::max()) - mesh->vertexOffset);
        texCoordMeshRanges[mesh->buffers.get()].push_back({ mesh->vertexOffset, uint32_t(numVertices) });
    }

    for (const auto& mesh : m_SceneGraph->GetMeshes())
    {
        auto buffers = mesh->buffers;

        if (!buffers)
            continue;

        if (!buffers->indexData.empty() && !buffers->indexBuffer)
        {
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.isIndexBuffer = true;
            bufferDesc.byteSize = buffers->indexData.size() * sizeof(uint32_t);
            bufferDesc.debugName = "IndexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.format = nvrhi::Format::R32_UINT;
            bufferDesc.isAccelStructBuildInput = m_RayTracingSupported;

            buffers->indexBuffer = m_Device->createBuffer(bufferDesc);

            if (m_DescriptorTable)
            {
                buffers->indexBufferDescriptor = std::make_shared<DescriptorHandle>(m_DescriptorTable->CreateDescriptorHandle(
                    nvrhi::BindingSetItem::RawBuffer_SRV(0, buffers->indexBuffer)));
            }

            commandList->beginTrackingBufferState(buffers->indexBuffer, nvrhi::ResourceStates::Common);

            commandList->writeBuffer(buffers->indexBuffer, buffers->indexData.data(), buffers->indexData.size() * sizeof(uint32_t));
            std::vector<uint32_t>().swap(buffers->indexData);

            nvrhi::ResourceStates state = nvrhi::ResourceStates::IndexBuffer | nvrhi::ResourceStates::ShaderResource;

            if (bufferDesc.isAccelStructBuildInput)
                state = state | nvrhi::ResourceStates::AccelStructBuildInput;

            commandList->setPermanentBufferState(buffers->indexBuffer, state);
            commandList->commitBarriers();
        }

        if (!buffers->vertexBuffer)
        {
            // Both UV streams share one format, including when either stream requires an FP32 fallback.
            if (buffers->texCoordFormat == TexCoordFormat::Float16
                && (!CanUseFloat16TexCoords(buffers->texcoord1Data) || !CanUseFloat16TexCoords(buffers->texcoord2Data)))
            {
                log::warning("Mesh '%s' has texture coordinates outside the finite Float16 range; using Float32 for its buffer group.",
                    mesh->name.c_str());
                buffers->texCoordFormat = TexCoordFormat::Float32;
            }
            else if (buffers->texCoordFormat == TexCoordFormat::Unorm16
                && !PrepareUnorm16TexCoords(*buffers, std::move(texCoordMeshRanges[buffers.get()])))
            {
                log::warning("Mesh '%s' has texture coordinates that cannot use finite UNORM16 decode bounds; using Float32 for its buffer group.",
                    mesh->name.c_str());
                buffers->texCoordFormat = TexCoordFormat::Float32;
            }

            nvrhi::BufferDesc bufferDesc;
            bufferDesc.isVertexBuffer = true;
            bufferDesc.byteSize = 0;
            bufferDesc.debugName = "VertexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.isAccelStructBuildInput = m_RayTracingSupported;

            if (!buffers->positionData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::Position), 
                    buffers->positionData.size() * sizeof(buffers->positionData[0]), bufferDesc.byteSize);
            }

            if (!buffers->normalData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::Normal),
                    buffers->normalData.size() * sizeof(buffers->normalData[0]), bufferDesc.byteSize);
            }

            if (!buffers->tangentData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::Tangent),
                    buffers->tangentData.size() * sizeof(buffers->tangentData[0]), bufferDesc.byteSize);
            }

            if (!buffers->texcoord1Data.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::TexCoord1),
                    buffers->texcoord1Data.size() * buffers->getTexCoordStride(), bufferDesc.byteSize);
            }

            if (!buffers->texcoord2Data.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::TexCoord2),
                    buffers->texcoord2Data.size() * buffers->getTexCoordStride(), bufferDesc.byteSize);
            }

            if (!buffers->weightData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::JointWeights),
                    buffers->weightData.size() * sizeof(buffers->weightData[0]), bufferDesc.byteSize);
            }

            if (!buffers->jointData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::JointIndices),
                    buffers->jointData.size() * sizeof(buffers->jointData[0]), bufferDesc.byteSize);
            }

            if (!buffers->radiusData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::CurveRadius),
                    buffers->radiusData.size() * sizeof(buffers->radiusData[0]), bufferDesc.byteSize);
            }

            if (bufferDesc.byteSize == 0)
            {
	            continue;
            }

            buffers->vertexBuffer = m_Device->createBuffer(bufferDesc);
            if (m_DescriptorTable)
            {
                buffers->vertexBufferDescriptor = std::make_shared<DescriptorHandle>(
                    m_DescriptorTable->CreateDescriptorHandle(nvrhi::BindingSetItem::RawBuffer_SRV(0, buffers->vertexBuffer)));
            }

            commandList->beginTrackingBufferState(buffers->vertexBuffer, nvrhi::ResourceStates::Common);

            if (!buffers->positionData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::Position);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->positionData.data(), range.byteSize, range.byteOffset);
                std::vector<float3>().swap(buffers->positionData);
            }

            if (!buffers->normalData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::Normal);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->normalData.data(), range.byteSize, range.byteOffset);
                std::vector<uint32_t>().swap(buffers->normalData);
            }

            if (!buffers->tangentData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::Tangent);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->tangentData.data(), range.byteSize, range.byteOffset);
                std::vector<uint32_t>().swap(buffers->tangentData);
            }

            WriteTexCoords(commandList, *buffers, VertexAttribute::TexCoord1, buffers->texcoord1Data);
            WriteTexCoords(commandList, *buffers, VertexAttribute::TexCoord2, buffers->texcoord2Data);

            if (!buffers->weightData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::JointWeights);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->weightData.data(), range.byteSize, range.byteOffset);
                std::vector<float4>().swap(buffers->weightData);
            }

            if (!buffers->jointData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::JointIndices);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->jointData.data(), range.byteSize, range.byteOffset);
                std::vector<vector<uint16_t, 4>>().swap(buffers->jointData);
            }

            if (!buffers->radiusData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::CurveRadius);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->radiusData.data(), range.byteSize, range.byteOffset);
                std::vector<float>().swap(buffers->radiusData);
            }

            nvrhi::ResourceStates state = nvrhi::ResourceStates::VertexBuffer | nvrhi::ResourceStates::ShaderResource;

            if (bufferDesc.isAccelStructBuildInput)
                state = state | nvrhi::ResourceStates::AccelStructBuildInput;

            commandList->setPermanentBufferState(buffers->vertexBuffer, state);
            commandList->commitBarriers();
        }
    }

    for (const auto& skinnedInstance : m_SceneGraph->GetSkinnedMeshInstances())
    {
        const auto& skinnedMesh = skinnedInstance->GetMesh();

        if (!skinnedMesh->buffers)
        {
            skinnedMesh->buffers = std::make_shared<BufferGroup>();

            uint32_t totalVertices = skinnedMesh->totalVertices;

            skinnedMesh->buffers->indexBuffer = skinnedInstance->GetPrototypeMesh()->buffers->indexBuffer;
            skinnedMesh->buffers->indexBufferDescriptor = skinnedInstance->GetPrototypeMesh()->buffers->indexBufferDescriptor;

            const auto& prototypeBuffers = skinnedInstance->GetPrototypeMesh()->buffers;
            const auto& skinnedBuffers = skinnedMesh->buffers;
            skinnedBuffers->texCoordFormat = prototypeBuffers->texCoordFormat;
            const uint32_t prototypeVertexOffset = skinnedInstance->GetPrototypeMesh()->vertexOffset;
            for (auto range : prototypeBuffers->texCoordDecodeRanges)
            {
                const uint64_t first = std::max(uint64_t(range.vertexOffset), uint64_t(prototypeVertexOffset));
                const uint64_t end = std::min(uint64_t(range.vertexOffset) + range.numVertices,
                    uint64_t(prototypeVertexOffset) + totalVertices);
                if (first >= end)
                    continue;
                range.vertexOffset = uint32_t(first - prototypeVertexOffset);
                range.numVertices = uint32_t(end - first);
                skinnedBuffers->texCoordDecodeRanges.push_back(range);
            }

            size_t skinnedVertexBufferSize = 0;
            assert(prototypeBuffers->hasAttribute(VertexAttribute::Position));

            AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::Position),
                totalVertices * sizeof(float3), skinnedVertexBufferSize);
    
            AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::PrevPosition),
                totalVertices * sizeof(float3), skinnedVertexBufferSize);
            
            if(prototypeBuffers->hasAttribute(VertexAttribute::Normal))
            {
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::Normal),
                    totalVertices * sizeof(uint32_t), skinnedVertexBufferSize);
            }

            if (prototypeBuffers->hasAttribute(VertexAttribute::Tangent))
            {
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::Tangent),
                    totalVertices * sizeof(uint32_t), skinnedVertexBufferSize);
            }

            if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord1))
            {
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord1),
                    totalVertices * skinnedBuffers->getTexCoordStride(), skinnedVertexBufferSize);
            }

            if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord2))
            {
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord2),
                    totalVertices * skinnedBuffers->getTexCoordStride(), skinnedVertexBufferSize);
            }

            nvrhi::BufferDesc bufferDesc;
            bufferDesc.isVertexBuffer = true;
            bufferDesc.byteSize = skinnedVertexBufferSize;
            bufferDesc.debugName = "SkinnedVertexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.canHaveUAVs = true;
            bufferDesc.isAccelStructBuildInput = m_RayTracingSupported;
            bufferDesc.keepInitialState = true;
            bufferDesc.initialState = nvrhi::ResourceStates::VertexBuffer;

            skinnedBuffers->vertexBuffer = m_Device->createBuffer(bufferDesc);

            if (m_DescriptorTable)
            {
                skinnedBuffers->vertexBufferDescriptor = std::make_shared<DescriptorHandle>(
                    m_DescriptorTable->CreateDescriptorHandle(nvrhi::BindingSetItem::RawBuffer_SRV(0, skinnedBuffers->vertexBuffer)));
            }
        }

        if (!skinnedInstance->jointBuffer)
        {
            nvrhi::BufferDesc jointBufferDesc;
            jointBufferDesc.debugName = "JointBuffer";
            jointBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            jointBufferDesc.keepInitialState = true;
            jointBufferDesc.canHaveRawViews = true;
            jointBufferDesc.byteSize = sizeof(dm::float4x4) * skinnedInstance->joints.size();
            skinnedInstance->jointBuffer = m_Device->createBuffer(jointBufferDesc);
        }

        if (!skinnedInstance->skinningBindingSet)
        {
            const auto& prototypeBuffers = skinnedInstance->GetPrototypeMesh()->buffers;
            const auto& skinnedBuffers = skinnedInstance->GetMesh()->buffers;
            
            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(SkinningConstants)),
                nvrhi::BindingSetItem::RawBuffer_SRV(0, prototypeBuffers->vertexBuffer),
                nvrhi::BindingSetItem::RawBuffer_SRV(1, skinnedInstance->jointBuffer),
                nvrhi::BindingSetItem::RawBuffer_UAV(0, skinnedBuffers->vertexBuffer)
            };

            skinnedInstance->skinningBindingSet = m_Device->createBindingSet(setDesc, m_SkinningBindingLayout);
        }
    }

    // Resolve after all shared buffers and remapped skinned ranges are finalized. Hints contain
    // indices rather than pointers or copied decode values, and readers always validate them.
    for (const auto& mesh : m_SceneGraph->GetMeshes())
    {
        for (const auto& geometry : mesh->geometries)
        {
            geometry->texCoordDecodeRangeIndex = mesh->buffers && mesh->buffers->texCoordFormat == TexCoordFormat::Unorm16
                ? mesh->buffers->getTexCoordDecodeRangeIndex(mesh->vertexOffset + geometry->vertexOffsetInMesh)
                : ~0u;
        }
    }
}

nvrhi::BufferHandle Scene::CreateMaterialBuffer()
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants) * m_Resources->materialData.size();
    bufferDesc.debugName = "BindlessMaterials";
    bufferDesc.structStride = sizeof(MaterialConstants);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return m_Device->createBuffer(bufferDesc);
}

nvrhi::BufferHandle Scene::CreateGeometryBuffer()
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(GeometryData) * m_Resources->geometryData.size();
    bufferDesc.debugName = "BindlessGeometry";
    bufferDesc.structStride = sizeof(GeometryData);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return m_Device->createBuffer(bufferDesc);
}

nvrhi::BufferHandle Scene::CreateInstanceBuffer()
{
    // On DX11, a buffer cannot be both structured and vertex.
    // On other APIs, a structured instance buffer can be used for rasterization.
    bool const needStructuredBuffer = m_Device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11;

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(InstanceData) * m_Resources->instanceData.size();
    bufferDesc.debugName = "Instances";
    bufferDesc.structStride = needStructuredBuffer ? sizeof(InstanceData) : 0;
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.isVertexBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return m_Device->createBuffer(bufferDesc);
}

nvrhi::BufferHandle Scene::CreateMaterialConstantBuffer(const std::string& debugName)
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants);
    bufferDesc.debugName = debugName;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;

    return m_Device->createBuffer(bufferDesc);
}

void Scene::WriteMaterialBuffer(nvrhi::ICommandList* commandList) const
{
    commandList->writeBuffer(m_MaterialBuffer, m_Resources->materialData.data(),
        m_Resources->materialData.size() * sizeof(MaterialConstants));
}

void Scene::WriteGeometryBuffer(nvrhi::ICommandList* commandList) const
{
    commandList->writeBuffer(m_GeometryBuffer, m_Resources->geometryData.data(),
        m_Resources->geometryData.size() * sizeof(GeometryData));
}

void Scene::WriteInstanceBuffer(nvrhi::ICommandList* commandList) const
{
    commandList->writeBuffer(m_InstanceBuffer, m_Resources->instanceData.data(), 
        m_Resources->instanceData.size() * sizeof(InstanceData));
}

void Scene::UpdateMaterial(const std::shared_ptr<Material>& material)
{
    material->FillConstantBuffer(m_Resources->materialData[material->materialID], m_UseResourceDescriptorHeapBindless);
}

void Scene::UpdateGeometry(const std::shared_ptr<MeshInfo>& mesh)
{
    // TODO: support 64-bit buffer offsets in the CB.
    for (const auto& geometry : mesh->geometries)
    {
        uint32_t indexOffset = mesh->indexOffset + geometry->indexOffsetInMesh;
        uint32_t vertexOffset = mesh->vertexOffset + geometry->vertexOffsetInMesh;

        GeometryData& gdata = m_Resources->geometryData[geometry->globalGeometryIndex];
        gdata.numIndices = geometry->numIndices;
        gdata.numVertices = geometry->numVertices;
        gdata.indexBufferIndex = mesh->buffers->indexBufferDescriptor ? mesh->buffers->indexBufferDescriptor->Get() : -1;
        gdata.indexOffset = indexOffset * sizeof(uint32_t);
        gdata.vertexBufferIndex = mesh->buffers->vertexBufferDescriptor ? mesh->buffers->vertexBufferDescriptor->Get() : -1;
        gdata.texCoordFormat = uint32_t(mesh->buffers->texCoordFormat);
        const auto& texCoordDecode = mesh->buffers->getTexCoordDecodeRange(vertexOffset, geometry->texCoordDecodeRangeIndex);
        gdata.texCoord1ScaleBias = float4(texCoordDecode.texCoord1.scale, texCoordDecode.texCoord1.offset);
        gdata.texCoord2ScaleBias = float4(texCoordDecode.texCoord2.scale, texCoordDecode.texCoord2.offset);
        gdata.positionOffset = mesh->buffers->hasAttribute(VertexAttribute::Position)
            ? uint32_t(vertexOffset * sizeof(float3) + mesh->buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset) : ~0u;
        gdata.prevPositionOffset = mesh->buffers->hasAttribute(VertexAttribute::PrevPosition)
            ? uint32_t(vertexOffset * sizeof(float3) + mesh->buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset) : ~0u;
        gdata.texCoord1Offset = mesh->buffers->hasAttribute(VertexAttribute::TexCoord1)
            ? uint32_t(vertexOffset * mesh->buffers->getTexCoordStride() + mesh->buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset) : ~0u;
        gdata.texCoord2Offset = mesh->buffers->hasAttribute(VertexAttribute::TexCoord2)
            ? uint32_t(vertexOffset * mesh->buffers->getTexCoordStride() + mesh->buffers->getVertexBufferRange(VertexAttribute::TexCoord2).byteOffset) : ~0u;
        gdata.normalOffset = mesh->buffers->hasAttribute(VertexAttribute::Normal)
            ? uint32_t(vertexOffset * sizeof(uint32_t) + mesh->buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset) : ~0u;
        gdata.tangentOffset = mesh->buffers->hasAttribute(VertexAttribute::Tangent)
            ? uint32_t(vertexOffset * sizeof(uint32_t) + mesh->buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset) : ~0u;
        gdata.curveRadiusOffset = mesh->buffers->hasAttribute(VertexAttribute::CurveRadius)
            ? uint32_t(vertexOffset * sizeof(float) + mesh->buffers->getVertexBufferRange(VertexAttribute::CurveRadius).byteOffset) : ~0u;
        gdata.materialIndex = geometry->material ? geometry->material->materialID : ~0u;
    }
}

GeometryData* Scene::GetGeometryData(const MeshGeometry& geometry) const
{
    if (m_Resources == nullptr || uint(geometry.globalGeometryIndex) >= m_Resources->geometryData.size() )
        return nullptr;

    return &m_Resources->geometryData[geometry.globalGeometryIndex];
}


void Scene::UpdateInstance(const std::shared_ptr<MeshInstance>& instance)
{
    SceneGraphNode* node = instance->GetNode();
    if (!node)
        return;

    InstanceData& idata = m_Resources->instanceData[instance->GetInstanceIndex()];
    affineToColumnMajor(node->GetLocalToWorldTransformFloat(), idata.transform);
    affineToColumnMajor(node->GetPrevLocalToWorldTransformFloat(), idata.prevTransform);

    const auto& mesh = instance->GetMesh();
    idata.firstGeometryInstanceIndex = instance->GetGeometryInstanceIndex();
    idata.numGeometries = uint32_t(mesh->geometries.size());
    idata.firstGeometryIndex = idata.numGeometries > 0 ? mesh->geometries[0]->globalGeometryIndex : -1;
    idata.flags = 0u;

    if (mesh->type == MeshType::CurveDisjointOrthogonalTriangleStrips)
    {
        idata.flags |= InstanceFlags_CurveDisjointOrthogonalTriangleStrips;
    }
    else if (mesh->type == MeshType::CurveLinearSweptSpheres)
    {
        // NVAPI does not support Vulkan, so NvRtIsLssHit() cannot be used to detect LSS hits.
        // Instead, we explicitly mark each LSS instance with a flag and check this flag during hit processing.
        // For consistency and completeness, this flag is also set for DX12.
        idata.flags |= InstanceFlags_CurveLinearSweptSpheres;
    }
}
