#include "WPShaderValueUpdater.hpp"
#include "WPNodeTransformResolver.hpp"
#include "Eigen/src/Core/Matrix.h"
#include "Eigen/src/Geometry/Transform.h"
#include "scene/Scene.h"
#include "scene/SceneImageEffectLayer.h"
#include "scene/SceneNode.h"
#include "audio/SoundManager.h"
#include "scenescript/WPSceneScriptHost.hpp"
#include "SpriteAnimation.hpp"
#include "SpecTexs.hpp"
#include "core/ArrayHelper.hpp"
#include "utils/Algorism.h"
#include "utils/Logging.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <iostream>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <limits>
#include <numeric>
#include <vector>
#include <sstream>

using namespace wallpaper;
using namespace Eigen;

namespace
{
constexpr float kDefaultMouseCoord = 0.5f;
constexpr double kParallaxSettleRatio = std::log(100.0);
constexpr std::array<uint32_t, 3> kAudioSpectrumResolutions { 16, 32, 64 };
constexpr std::array<const char*, 3> kAudioSpectrumLeftUniforms {
    "g_AudioSpectrum16Left",
    "g_AudioSpectrum32Left",
    "g_AudioSpectrum64Left",
};
constexpr std::array<const char*, 3> kAudioSpectrumRightUniforms {
    "g_AudioSpectrum16Right",
    "g_AudioSpectrum32Right",
    "g_AudioSpectrum64Right",
};

std::vector<float> PeakResampleSpectrum(std::span<const float> values, uint32_t resolution) {
    const size_t       target_size = static_cast<size_t>(resolution);
    std::vector<float> result(target_size, 0.0f);
    if (target_size == 0 || values.empty()) return result;

    const size_t source_size = values.size();
    if (target_size == source_size) {
        std::copy(values.begin(), values.end(), result.begin());
        return result;
    }

    if (target_size > source_size) {
        for (size_t i = 0; i < target_size; ++i) {
            const size_t source_index = std::min(source_size - 1, (i * source_size) / target_size);
            result[i]                 = values[source_index];
        }
        return result;
    }

    for (size_t i = 0; i < target_size; ++i) {
        const size_t begin = (i * source_size) / target_size;
        const size_t end   = std::max(begin + 1, ((i + 1) * source_size) / target_size);
        float        peak  = 0.0f;
        for (size_t j = begin; j < std::min(end, source_size); ++j) {
            peak = std::max(peak, values[j]);
        }
        result[i] = peak;
    }
    return result;
}

struct MeshBounds2D {
    bool     valid { false };
    Vector3d center { Vector3d::Zero() };
    Vector2d halfExtent { Vector2d::Ones() };
};

std::string NodeDebugLabel(const SceneNode* node) {
    if (node == nullptr) return "<null>";
    std::ostringstream oss;
    oss << "name='" << node->Name() << "'"
        << " localVisible=" << (node->LocalVisible() ? "true" : "false")
        << " layerVisible=" << (node->LayerVisible() ? "true" : "false")
        << " visible=" << (node->Visible() ? "true" : "false");
    if (node->Parent() != nullptr) {
        oss << " parent='" << node->Parent()->Name() << "'";
    }
    return oss.str();
}

float SanitizeMouseCoord(double value) {
    if (! std::isfinite(value)) return kDefaultMouseCoord;
    return std::clamp(static_cast<float>(value), 0.0f, 1.0f);
}

MeshBounds2D ComputeMeshBounds2D(const SceneMesh* mesh) {
    if (mesh == nullptr || mesh->VertexCount() == 0) return {};

    const auto& vertexArray = mesh->GetVertexArray(0);
    if (vertexArray.VertexCount() == 0) return {};

    const auto attrOffsets = vertexArray.GetAttrOffsetMap();
    if (!exists(attrOffsets, std::string(WE_IN_POSITION))) return {};

    const auto& posAttr     = attrOffsets.at(std::string(WE_IN_POSITION));
    const auto  components  = SceneVertexArray::TypeCount(posAttr.attr.type);
    const auto  stride      = vertexArray.OneSize();
    const auto  offset      = posAttr.offset / sizeof(float);
    const auto* vertexData  = vertexArray.Data();
    const auto  vertexCount = vertexArray.VertexCount();
    if (vertexData == nullptr || components < 2) return {};

    Vector3d minPos(std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity());
    Vector3d maxPos(-std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity());

    for (usize i = 0; i < vertexCount; ++i) {
        const auto base = i * stride + offset;
        const auto x    = static_cast<double>(vertexData[base + 0]);
        const auto y    = static_cast<double>(vertexData[base + 1]);
        const auto z    = components >= 3 ? static_cast<double>(vertexData[base + 2]) : 0.0;
        minPos = minPos.cwiseMin(Vector3d(x, y, z));
        maxPos = maxPos.cwiseMax(Vector3d(x, y, z));
    }

    const auto center     = (minPos + maxPos) * 0.5;
    const auto halfExtent = Vector2d(std::max((maxPos.x() - minPos.x()) * 0.5, 1e-6),
                                     std::max((maxPos.y() - minPos.y()) * 0.5, 1e-6));
    return MeshBounds2D { .valid = true, .center = center, .halfExtent = halfExtent };
}

bool IsModelRenderNode(SceneNode* node) {
    auto* mesh = node != nullptr ? node->Mesh() : nullptr;
    const auto* material = mesh != nullptr ? mesh->Material() : nullptr;
    // `g_EyePosition` updates are scoped to materials explicitly marked by WPModelObject
    // materialization. This prevents the new 3D camera uniform support from changing any legacy 2D
    // image, effect, text, or particle shader that happens to declare the same uniform name.
    return material != nullptr && material->modelRenderState.has_value();
}

bool IsZeroParallaxDepth(const std::array<float, 2>& depth) {
    return std::abs(depth[0]) <= 1e-6f && std::abs(depth[1]) <= 1e-6f;
}

ShaderValue ToDxcCBufferMatrixUniform(const Matrix4d& matrix) {
    // The DXC WE prologue maps authored `mul(v, M)` to native `mul(M, v)` so shader code observes
    // the same column-vector transform contract as the renderer. Keep Eigen's column-major matrix
    // bytes untouched; changing layout here would make uniform upload policy depend on the source
    // spelling of every shader expression instead of on the single language bridge in WPShaderParser.
    return ShaderValue::fromMatrix(matrix.cast<float>());
}

ShaderValue ToDxcRowVectorSkinningUniform(std::span<const Affine3f> matrices) {
    // WE authors skinning uniforms as GLSL `mat4x3`: four columns (xyz + translation) by three
    // rows, then multiplies `mul(float4(position, 1), g_Bones[i])` to get xyz. The DXC bridge spells
    // that type as HLSL `float3x4` and swaps the multiply to native `mul(M, v)`, which SPIR-V lowers
    // as a row-major matrix with ArrayStride 64. Therefore each bone needs four std140 vec4 slots:
    // the affine matrix's x/y/z rows in the first three lanes, plus a padded fourth lane that remains
    // zero. Packing compact 12-float matrices here misaligns every bone after the first one.
    std::vector<float> packed;
    packed.reserve(matrices.size() * 16);
    for (const auto& affine : matrices) {
        const Matrix4f matrix = affine.matrix();
        for (int column = 0; column < 4; ++column) {
            packed.push_back(matrix(0, column));
            packed.push_back(matrix(1, column));
            packed.push_back(matrix(2, column));
            packed.push_back(0.0f);
        }
    }
    return ShaderValue(std::span<const float>(packed.data(), packed.size()));
}

SceneNode* RemapSceneNodeReference(SceneNode* node, SceneNode* old_node, SceneNode* new_node) {
    return node == old_node ? new_node : node;
}

void PreserveDeferredRuntimeParallaxContract(const WPShaderValueData& old_data,
                                             WPShaderValueData&       new_data,
                                             SceneNode*               old_node,
                                             SceneNode*               new_node) {
    bool preserved = false;

    if (IsZeroParallaxDepth(new_data.parallaxDepth) &&
        ! IsZeroParallaxDepth(old_data.parallaxDepth)) {
        // Hidden logical placeholders can receive importer-side parallax repairs after the initial
        // parse, especially compose/effect containers whose authored JSON omits parallaxDepth but
        // whose descendants must still use the container as a camera-parallax anchor. Runtime
        // materialization reparses the real layer from JSON, so copy only this repaired parallax
        // contract when the replacement still has no authored depth of its own.
        new_data.parallaxDepth = old_data.parallaxDepth;
        preserved = true;
    }

    if (new_data.parallax_anchor == nullptr && old_data.parallax_anchor != nullptr) {
        new_data.parallax_anchor =
            RemapSceneNodeReference(old_data.parallax_anchor, old_node, new_node);
        preserved = true;
    }

    if (preserved && old_data.suppress_model_parallax) {
        new_data.suppress_model_parallax = true;
    }
}

Matrix4d ComputeEffectTextureProjection(const SceneNode* projectionNode,
                                        const SceneMesh* projectionMesh,
                                        const Matrix4d&  projectionModelTrans,
                                        const Matrix4d&  viewProjectionTrans) {
    if (projectionNode == nullptr || projectionMesh == nullptr) return Matrix4d::Identity();

    const auto bounds = ComputeMeshBounds2D(projectionMesh);
    if (!bounds.valid) return viewProjectionTrans * projectionModelTrans;

    const auto localFromNormalized =
        (Affine3d(Eigen::Translation3d(bounds.center)) *
         Eigen::Scaling(bounds.halfExtent.x(), bounds.halfExtent.y(), 1.0))
            .matrix();
    return viewProjectionTrans * projectionModelTrans * localFromNormalized;
}

std::string_view ResolveEffectiveNodeCameraName(const SceneNode* node) {
    // Effect-backed text still contributes intermediate bridge-source quads to the generic image
    // path while the logical text owner carries the camera binding. Walking ancestors here lets
    // those bridge-source quads inherit the same offscreen camera contract as the owning text
    // primitive, so text effects stay synchronized without any text-specific fallback camera path.
    for (auto* current = node; current != nullptr; current = current->Parent()) {
        if (!current->Camera().empty()) return current->Camera();
    }
    return {};
}

} // namespace

void WPShaderValueUpdater::FrameBegin() {
    m_puppet_frame_serial++;
    m_modelTransformCache.clear();
    m_parallaxOffsetCache.clear();
    m_attachmentTransformCache.clear();
    // 3D model camera paths are sampled before uniforms so the model-only camera projection,
    // g_EyePosition, and view-basis uniforms all describe the same frame. Scenes without model
    // camera paths return immediately inside Scene and keep the legacy 2D path untouched.
    if (m_scene != nullptr) {
        m_scene->UpdateModelCameraPath();
    }
    /*
        using namespace std::chrono;
        auto nowTime = system_clock::to_time_t(system_clock::now());
        auto cTime   = std::localtime(&nowTime);
        m_dayTime =
            (((cTime->tm_hour * 60) + cTime->tm_min) * 60 + cTime->tm_sec) / (24.0f * 60.0f
       * 60.0f);
    */
    const std::array<float, 2> previousMousePos = m_mousePos;
    if (!(m_parallax.delay > 0.0f) || !std::isfinite(m_parallax.delay)) {
        m_mousePosLast     = previousMousePos;
        m_mousePos         = m_mousePosInput;
        AdvanceAllPuppets();
        return;
    }

    const double frameTime = std::max(m_scene->frameTime, 0.0);
    const double t =
        1.0 - std::exp(-(frameTime * kParallaxSettleRatio) / static_cast<double>(m_parallax.delay));
    m_mousePosLast     = previousMousePos;
    m_mousePos         = std::array { (float)algorism::lerp(t, m_mousePos[0], m_mousePosInput[0]),
                                      (float)algorism::lerp(t, m_mousePos[1], m_mousePosInput[1]) };
    AdvanceAllPuppets();
}

void WPShaderValueUpdater::AdvanceAllPuppets() {
    if (!m_scene) return;
    const double frame_time = m_scene->frameTime;

    for (auto& [addr, nodeData] : m_nodeDataMap) {
        if (!nodeData.puppet_layer.hasPuppet()) continue;
        nodeData.puppet_layer.AdvanceIfNeeded(frame_time, m_puppet_frame_serial);

        if (nodeData.transform_binding.IsBoneAttachment()) {
            auto* parent = nodeData.transform_binding.parent;
            if (parent != nullptr) {
                auto parent_it = m_nodeDataMap.find(parent);
                if (parent_it != m_nodeDataMap.end() &&
                    parent_it->second.puppet_layer.hasPuppet()) {
                    parent_it->second.puppet_layer.AdvanceIfNeeded(
                        frame_time, m_puppet_frame_serial);
                }
            }
        }
    }
}

void WPShaderValueUpdater::FrameEnd() {}

void WPShaderValueUpdater::MouseInput(double x, double y) {
    m_mousePosInput[0] = SanitizeMouseCoord(x);
    m_mousePosInput[1] = SanitizeMouseCoord(y);
}

void WPShaderValueUpdater::InitUniforms(SceneNode* pNode, const ExistsUniformOp& existsOp) {
    m_nodeUniformInfoMap[pNode] = WPUniformInfo();
    auto& info                  = m_nodeUniformInfoMap[pNode];
    info.has_MI                 = existsOp(G_MI);
    info.has_M                  = existsOp(G_M);
    info.has_AM                 = existsOp(G_AM);
    info.has_MVP                = existsOp(G_MVP);
    info.has_LMM                = existsOp(G_LMM);
    info.has_EMVP               = existsOp(G_EMVP);
    info.has_MVPI               = existsOp(G_MVPI);
    info.has_ETVP               = existsOp(G_ETVP);
    info.has_ETVPI              = existsOp(G_ETVPI);

    info.has_VP = existsOp(G_VP);

    info.has_BONES            = existsOp(G_BONES);
    info.has_TIME             = existsOp(G_TIME);
    info.has_DAYTIME          = existsOp(G_DAYTIME);
    info.has_FRAMETIME        = existsOp(G_FRAMETIME);
    info.has_POINTERPOSITION  = existsOp(G_POINTERPOSITION);
    info.has_POINTERPOSITIONLAST = existsOp(G_POINTERPOSITIONLAST);
    info.has_POINTERSTATE     = existsOp(G_POINTERSTATE);
    info.has_PARALLAXPOSITION = existsOp(G_PARALLAXPOSITION);
    info.has_TEXELSIZE        = existsOp(G_TEXELSIZE);
    info.has_TEXELSIZEHALF    = existsOp(G_TEXELSIZEHALF);
    info.has_SCREEN           = existsOp(G_SCREEN);
    info.has_LP               = existsOp(G_LP);
    info.has_model_LCP        = IsModelRenderNode(pNode) && existsOp(G_LCP);
    info.has_LCR              = IsModelRenderNode(pNode) && existsOp(G_LCR);
    info.has_EYE_POSITION     = IsModelRenderNode(pNode) && existsOp(G_EYE_POSITION);
    info.has_VIEWUP           = IsModelRenderNode(pNode) && existsOp(G_VIEWUP);
    info.has_VIEWRIGHT        = IsModelRenderNode(pNode) && existsOp(G_VIEWRIGHT);
    info.has_VIEWFORWARD      = IsModelRenderNode(pNode) && existsOp(G_VIEWFORWARD);
    for (size_t index = 0; index < kAudioSpectrumResolutions.size(); index++) {
        info.has_audio_spectrum_left[index] = existsOp(kAudioSpectrumLeftUniforms[index]);
        info.has_audio_spectrum_right[index] = existsOp(kAudioSpectrumRightUniforms[index]);
    }

    std::accumulate(begin(info.texs), end(info.texs), 0, [&existsOp](uint index, auto& value) {
        value.has_resolution = existsOp(WE_GLTEX_RESOLUTION_NAMES[index]);
        value.has_mipmap     = existsOp(WE_GLTEX_MIPMAPINFO_NAMES[index]);
        return index + 1;
    });

    if (pNode != nullptr && WallpaperDebugLayerEnabled(pNode->ID())) {
        std::ostringstream oss;
        oss << "left=[";
        for (size_t index = 0; index < kAudioSpectrumResolutions.size(); index++) {
            if (index != 0) oss << ",";
            oss << kAudioSpectrumResolutions[index] << ":"
                << (info.has_audio_spectrum_left[index] ? "1" : "0");
        }
        oss << "] right=[";
        for (size_t index = 0; index < kAudioSpectrumResolutions.size(); index++) {
            if (index != 0) oss << ",";
            oss << kAudioSpectrumResolutions[index] << ":"
                << (info.has_audio_spectrum_right[index] ? "1" : "0");
        }
        oss << "]";
        LOG_INFO("DebugSpectrumUniformInit: layer=%d node=%s %s",
                 pNode->ID(),
                 NodeDebugLabel(pNode).c_str(),
                 oss.str().c_str());
    }
}

void WPShaderValueUpdater::UpdateUniforms(SceneNode* pNode, sprite_map_t& sprites,
                                          const UpdateUniformOp& updateOp,
                                          const ShaderUniformOverrides* overrides) {
    const auto node_cam_name = ResolveEffectiveNodeCameraName(pNode);
    const bool use_active_camera_for_uniforms =
        overrides != nullptr && overrides->use_active_camera_for_uniforms;
    const bool has_named_camera_override =
        overrides != nullptr && overrides->use_camera_override && !overrides->camera_name.empty();
    const bool has_camera_override = use_active_camera_for_uniforms || has_named_camera_override;
    const std::string_view uniform_cam_name =
        use_active_camera_for_uniforms
            ? std::string_view {}
            : (has_named_camera_override ? overrides->camera_name : node_cam_name);

    const SceneCamera* camera;
    if (! uniform_cam_name.empty()) {
        auto camera_it = m_scene->cameras.find(std::string(uniform_cam_name));
        if (camera_it != m_scene->cameras.end()) {
            camera = camera_it->second.get();
        } else {
            LOG_ERROR("ShaderUniformCameraOverride: camera '%.*s' not found for node '%s'",
                      static_cast<int>(uniform_cam_name.size()),
                      uniform_cam_name.data(),
                      pNode != nullptr ? pNode->Name().c_str() : "<null>");
            camera = m_scene->activeCamera;
        }
    } else {
        camera = m_scene->activeCamera;
    }

    if (! camera) return;

    const bool use_active_parallax_camera =
        has_camera_override && overrides->use_active_camera_for_parallax &&
        m_scene->activeCamera != nullptr;
    const SceneCamera* model_parallax_camera =
        use_active_parallax_camera ? m_scene->activeCamera : camera;
    const bool use_camera_local_transform_caches =
        has_camera_override && model_parallax_camera != m_scene->activeCamera;

    Map<void*, Matrix4d> localModelTransformCache;
    Map<void*, Vector3f> localParallaxOffsetCache;
    Map<void*, Affine3f> localAttachmentTransformCache;
    auto& modelTransformCache =
        use_camera_local_transform_caches ? localModelTransformCache : m_modelTransformCache;
    auto& parallaxOffsetCache =
        use_camera_local_transform_caches ? localParallaxOffsetCache : m_parallaxOffsetCache;
    auto& attachmentTransformCache =
        use_camera_local_transform_caches ? localAttachmentTransformCache
                                          : m_attachmentTransformCache;

    WPNodeTransformResolver transformResolver(*m_scene,
                                              m_parallax,
                                              m_nodeDataMap,
                                              modelTransformCache,
                                              parallaxOffsetCache,
                                              attachmentTransformCache,
                                              use_camera_local_transform_caches
                                                  ? model_parallax_camera
                                                  : m_scene->activeCamera,
                                              m_mousePos,
                                              m_puppet_frame_serial);

    if (exists(m_nodeDataMap, pNode)) {
        auto& nodeData = m_nodeDataMap.at(pNode);
        transformResolver.UpdateAttachmentParentIfNeeded(nodeData);
        auto localTransform = transformResolver.ResolveAttachmentLocalTransform(pNode);
        if (localTransform.has_value()) {
            SceneImageEffectLayer* effectLayer { nullptr };
            if (!node_cam_name.empty()) {
                auto camera_it = m_scene->cameras.find(std::string(node_cam_name));
                if (camera_it != m_scene->cameras.end() && camera_it->second->HasImgEffect()) {
                    effectLayer = camera_it->second->GetImgEffect().get();
                }
            }

            if (effectLayer != nullptr) {
                if (auto* worldNode = effectLayer->WorldNode()) {
                    worldNode->SetLocalAffine(*localTransform);
                    worldNode->UpdateTrans();
                    effectLayer->SyncResolvedNodeToWorld();
                }
            } else {
                pNode->SetLocalAffine(*localTransform);
            }
        }
    }

    pNode->UpdateTrans();

    if (! node_cam_name.empty()) {
        auto camera_it = m_scene->cameras.find(std::string(node_cam_name));
        if (camera_it != m_scene->cameras.end() && camera_it->second->HasImgEffect()) {
            auto* effectLayer = camera_it->second->GetImgEffect().get();
            auto* worldNode   = effectLayer->WorldNode();
            if (worldNode != nullptr && exists(m_nodeDataMap, worldNode)) {
                auto& worldNodeData = m_nodeDataMap.at(worldNode);
                transformResolver.UpdateAttachmentParentIfNeeded(worldNodeData);
                if (worldNodeData.IsBoneAttached()) {
                    // Effect-backed layers draw their source into a private camera, then composite a
                    // detached final node back into the visible scene. When a deferred hidden layer is
                    // later promoted to a real bone attachment, the visible world node no longer gets a
                    // normal SceneNode tree update that would carry the puppet bone and inherited
                    // parallax into that final writer. Resolve the attachment here before syncing the
                    // effect output matrix so runtime-revealed puppet parts keep the same camera
                    // parallax as layers that were visible at parse time.
                    auto localTransform = transformResolver.ResolveAttachmentLocalTransform(worldNode);
                    if (localTransform.has_value()) {
                        worldNode->SetLocalAffine(*localTransform);
                        worldNode->UpdateTrans();
                    }
                }
                if (worldNodeData.InheritsSceneParentTransform() || worldNodeData.IsBoneAttached()) {
                    const SceneCamera* displayCamera =
                        m_scene->activeCamera != nullptr ? m_scene->activeCamera : camera;
                    // Composition source routes publish a child layer's private authored effect
                    // through the neutral final composite. Keep that publisher on the raw routed
                    // world transform here; the actual compose-source pass applies the chosen
                    // source camera and camera-parallax exactly once. Syncing it to the active
                    // screen camera here would bake one parallax offset into the node transform,
                    // then the compose pass would add another one, which pulls effect-backed body
                    // parts away from direct siblings such as faces and eyes.
                    const auto worldModel =
                        effectLayer->PublishesPrivateFinalComposite()
                            ? transformResolver.ResolveRawModelTransform(worldNode)
                            : transformResolver.ResolveParallaxedModelTransform(
                                  worldNode, displayCamera, displayCamera != nullptr);
                    effectLayer->SyncResolvedNodeToMatrix(Affine3f(worldModel.cast<float>()));
                }
            }
        }
    }

    // Text is now allowed to be a first-class renderable without a backing SceneMesh material.
    // The old updater returned early here, which made transform uniforms unavailable to any
    // render path that was not disguised as a mesh/custom-shader node. Keeping material access
    // optional lets the dedicated text pass reuse the same attachment/parallax/camera transform
    // logic while still skipping mesh-only material uniform work when no mesh exists.
    auto* material = pNode->Mesh() != nullptr ? pNode->Mesh()->Material() : nullptr;
    // auto& shadervs = material->customShader.updateValueList;
    // const auto& valueSet = material->customShader.valueSet;

    assert(exists(m_nodeUniformInfoMap, pNode));
    const auto& info = m_nodeUniformInfoMap[pNode];

    bool hasNodeData = exists(m_nodeDataMap, pNode);
    if (hasNodeData) {
        auto& nodeData = m_nodeDataMap.at(pNode);
        for (const auto& el : nodeData.renderTargets) {
            if (m_scene->renderTargets.count(el.second) == 0) continue;
            const auto& rt = m_scene->renderTargets[el.second];

            const auto& unifrom_tex = info.texs[el.first];

            if (unifrom_tex.has_resolution) {
                // Runtime render targets expose one canonical resolution contract through
                // `ResolutionVector()`: physical size in `.xy`, logical content size in `.zw`.
                // Uniform updates should always forward that authoritative scene-side contract
                // directly instead of layering text-specific interpretation on top of it.
                std::array<i32, 4> resolution_uint(rt.ResolutionVector());
                updateOp(WE_GLTEX_RESOLUTION_NAMES[el.first],
                         ShaderValue(array_cast<float>(resolution_uint)));
            }
            if (unifrom_tex.has_mipmap) {
                updateOp(WE_GLTEX_MIPMAPINFO_NAMES[el.first], (float)rt.mipmap_level);
            }
        }
        if (nodeData.puppet_layer.hasPuppet() && info.has_BONES) {
            auto data = nodeData.puppet_layer.AdvanceIfNeeded(m_scene->frameTime, m_puppet_frame_serial);
            if (m_scene->scriptHost) {
                m_scene->scriptHost->NotifyAnimationLayersAdvanced(pNode);
            }
            updateOp(G_BONES, ToDxcRowVectorSkinningUniform(data));
        }
    }

    bool reqMI    = info.has_MI;
    bool reqM     = info.has_M;
    bool reqAM    = info.has_AM;
    bool reqMVP   = info.has_MVP;
    bool reqLMM   = info.has_LMM;
    bool reqEMVP  = info.has_EMVP;
    bool reqMVPI  = info.has_MVPI;
    bool reqETVP  = info.has_ETVP;
    bool reqETVPI = info.has_ETVPI;

    Matrix4d viewProTrans = camera->GetViewProjectionMatrix();

    if (info.has_VP) {
        updateOp(G_VP, ToDxcCBufferMatrixUniform(viewProTrans));
    }
    if (reqM || reqMVP || reqLMM || reqEMVP || reqMI || reqMVPI || reqETVP || reqETVPI) {
        Matrix4d modelTrans =
            transformResolver.ResolveParallaxedModelTransform(
                pNode, model_parallax_camera, uniform_cam_name != "effect");
        if (use_active_parallax_camera) {
            const auto source_camera_node = camera->GetAttachedNode();
            const auto source_camera_data =
                source_camera_node != nullptr && exists(m_nodeDataMap, source_camera_node.get())
                    ? &m_nodeDataMap.at(source_camera_node.get())
                    : nullptr;
            if (source_camera_data != nullptr && source_camera_data->AppliesModelParallax()) {
                // Composition-source routes project a child through the source camera, then the
                // parent composition layer publishes that source texture through its own final
                // scene-space writer. The child model therefore needs active-camera parallax minus
                // the parallax already represented by the source camera's attached layer; otherwise
                // parent depth is added twice for authored parallax groups, while source-camera
                // relative parallax drops the root fallback movement for groups whose final writer
                // deliberately suppresses its own model parallax.
                const auto source_parallax =
                    transformResolver.ResolveParallaxOffset(source_camera_node.get(),
                                                            model_parallax_camera);
                modelTrans =
                    Affine3d(Eigen::Translation3d((-source_parallax).cast<double>())).matrix() *
                    modelTrans;
            }
        }

        if (reqM) updateOp(G_M, ToDxcCBufferMatrixUniform(modelTrans));
        if (reqAM) updateOp(G_AM, ToDxcCBufferMatrixUniform(modelTrans));
        if (reqLMM) updateOp(G_LMM, ToDxcCBufferMatrixUniform(modelTrans));
        if (reqMI) updateOp(G_MI, ToDxcCBufferMatrixUniform(modelTrans.inverse()));
        if (reqMVP || reqEMVP) {
            Matrix4d mvpTrans = viewProTrans * modelTrans;
            if (reqMVP) updateOp(G_MVP, ToDxcCBufferMatrixUniform(mvpTrans));
            if (reqEMVP) updateOp(G_EMVP, ToDxcCBufferMatrixUniform(mvpTrans));
            if (reqMVPI) updateOp(G_MVPI, ToDxcCBufferMatrixUniform(mvpTrans.inverse()));
        }
        if (reqETVP || reqETVPI) {
            const SceneNode* projectionNode      = pNode;
            const SceneMesh* projectionMesh      = pNode->Mesh();
            Matrix4d         projectionModelTrans = modelTrans;
            Matrix4d         projectionViewPro    = viewProTrans;

            const WPShaderValueData* nodeDataPtr = hasNodeData ? &m_nodeDataMap.at(pNode) : nullptr;
            if (nodeDataPtr != nullptr && nodeDataPtr->effect_projection_node != nullptr &&
                nodeDataPtr->effect_projection_mesh != nullptr && m_scene->activeCamera != nullptr) {
                projectionNode = nodeDataPtr->effect_projection_node;
                projectionMesh = nodeDataPtr->effect_projection_mesh;
                const_cast<SceneNode*>(projectionNode)->UpdateTrans();
                projectionModelTrans = projectionNode->ModelTrans();
                projectionViewPro    = m_scene->activeCamera->GetViewProjectionMatrix();
            }

            const auto etvpTrans = ComputeEffectTextureProjection(projectionNode,
                                                                  projectionMesh,
                                                                  projectionModelTrans,
                                                                  projectionViewPro);
            if (reqETVP) updateOp(G_ETVP, ToDxcCBufferMatrixUniform(etvpTrans));
            if (reqETVPI) {
                if (std::abs(etvpTrans.determinant()) > 1e-12) {
                    updateOp(G_ETVPI, ToDxcCBufferMatrixUniform(etvpTrans.inverse()));
                } else {
                    updateOp(G_ETVPI, ToDxcCBufferMatrixUniform(Matrix4d::Identity()));
                }
            }
        }
    }

    //	g_EffectTextureProjectionMatrix
    // shadervs.push_back({"g_EffectTextureProjectionMatrixInverse",
    // ShaderValue::ValueOf(Eigen::Matrix4f::Identity())});
    if (info.has_TIME) updateOp(G_TIME, (float)m_scene->elapsingTime);

    if (info.has_DAYTIME) updateOp(G_DAYTIME, (float)m_dayTime);

    if (info.has_POINTERPOSITION) updateOp(G_POINTERPOSITION, m_mousePos);
    if (info.has_POINTERPOSITIONLAST) updateOp(G_POINTERPOSITIONLAST, m_mousePosLast);
    if (info.has_POINTERSTATE) {
        // Wallpaper Engine cursor ripple shaders treat `.z` as the left-button impulse term. Keep
        // the other lanes neutral because their exact editor-side meanings are effect-specific, and
        // writing arbitrary non-zero values would inject force into authored feedback buffers.
        updateOp(G_POINTERSTATE,
                 std::array<float, 4> { 0.0f, 0.0f, m_scene->cursorLeftDown ? 1.0f : 0.0f, 0.0f });
    }
    if (info.has_FRAMETIME) {
        // Feedback effects such as cursor ripple integrate per-frame decay from this uniform. The
        // parser already exposes the authored default, but runtime updates must overwrite it so the
        // simulation sees the same frame delta that drives timers and scripts.
        updateOp(G_FRAMETIME, static_cast<float>(std::max(m_scene->frameTime, 0.0)));
    }

    if (info.has_TEXELSIZE) updateOp(G_TEXELSIZE, m_texelSize);

    if (info.has_TEXELSIZEHALF)
        updateOp(G_TEXELSIZEHALF, std::array { m_texelSize[0] / 2.0f, m_texelSize[1] / 2.0f });

    if (info.has_SCREEN)
        updateOp(G_SCREEN,
                 std::array<float, 3> {
                     m_screen_size[0], m_screen_size[1], m_screen_size[0] / m_screen_size[1] });

    if (info.has_EYE_POSITION || info.has_VIEWUP || info.has_VIEWRIGHT || info.has_VIEWFORWARD) {
        // These camera basis uniforms are gated by IsModelRenderNode() during InitUniforms. Updating
        // them here gives 3D model shaders coherent camera-path lighting/reflection data without
        // introducing a new uniform contract for unrelated 2D image/effect/particle shaders.
        const auto eye = camera->GetPosition().cast<float>();
        Vector3f forward = camera->GetDirection().cast<float>();
        if (forward.norm() > 1e-6f) forward.normalize();
        Vector3f up = camera->GetUp().cast<float>();
        if (up.norm() > 1e-6f) up.normalize();
        Vector3f right = forward.cross(up);
        if (right.norm() > 1e-6f) right.normalize();

        if (info.has_EYE_POSITION)
            updateOp(G_EYE_POSITION, std::array<float, 3> { eye.x(), eye.y(), eye.z() });
        if (info.has_VIEWUP)
            updateOp(G_VIEWUP, std::array<float, 3> { up.x(), up.y(), up.z() });
        if (info.has_VIEWRIGHT)
            updateOp(G_VIEWRIGHT, std::array<float, 3> { right.x(), right.y(), right.z() });
        if (info.has_VIEWFORWARD)
            updateOp(G_VIEWFORWARD,
                     std::array<float, 3> { forward.x(), forward.y(), forward.z() });
    }

    if (info.has_PARALLAXPOSITION) {
        Vector2f para { 0.5f, 0.5f };
        if (m_parallax.enable) {
            const Vector2f mouseCentered = Vector2f(&m_mousePos[0]) - Vector2f { 0.5f, 0.5f };
            para = Vector2f { 0.5f, 0.5f } +
                   (Scaling(1.0f, -1.0f) * mouseCentered) * m_parallax.mouseinfluence;
        }
        updateOp(G_PARALLAXPOSITION, std::array { para[0], para[1] });
    }

    const bool requests_audio =
        std::any_of(info.has_audio_spectrum_left.begin(),
                    info.has_audio_spectrum_left.end(),
                    [](bool value) { return value; }) ||
        std::any_of(info.has_audio_spectrum_right.begin(),
                    info.has_audio_spectrum_right.end(),
                    [](bool value) { return value; });
    if (requests_audio) {
        std::vector<float> source_left;
        std::vector<float> source_right;
        std::vector<float> source_average;
        bool has_audio = false;
        if (m_scene->scriptHost != nullptr) {
            has_audio = m_scene->scriptHost->GetAudioSpectrum(
                kAudioSpectrumResolutions.back(), &source_left, &source_right, &source_average);
        } else if (m_scene->soundManager != nullptr) {
            m_scene->soundManager->GetSpectrum(
                kAudioSpectrumResolutions.back(), &source_left, &source_right, &source_average);
            has_audio = !source_left.empty() || !source_right.empty() || !source_average.empty();
        }
        if (! has_audio) {
            if (pNode != nullptr && WallpaperDebugLayerEnabled(pNode->ID())) {
                LOG_INFO("DebugSpectrumUniformUpdate: layer=%d node=%s resolution=%u has-audio=false "
                         "source=%s left-uniform=%s right-uniform=%s",
                         pNode->ID(),
                         NodeDebugLabel(pNode).c_str(),
                         kAudioSpectrumResolutions.back(),
                         m_scene->scriptHost != nullptr ? "scriptHost"
                         : m_scene->soundManager != nullptr ? "soundManager"
                                                            : "none",
                         requests_audio ? "true" : "false",
                         requests_audio ? "true" : "false");
            }
        } else {
            for (size_t index = 0; index < kAudioSpectrumResolutions.size(); index++) {
                if (! info.has_audio_spectrum_left[index] && ! info.has_audio_spectrum_right[index])
                    continue;

                const auto resolution = kAudioSpectrumResolutions[index];
                auto       left       = PeakResampleSpectrum(source_left, resolution);
                auto       right      = PeakResampleSpectrum(source_right, resolution);

                if (pNode != nullptr && WallpaperDebugLayerEnabled(pNode->ID())) {
                    LOG_INFO("DebugSpectrumUniformUpdate: layer=%d node=%s resolution=%u has-audio=true "
                             "source=%s left-uniform=%s right-uniform=%s left.size=%zu right.size=%zu "
                             "left0=%g left1=%g left2=%g left3=%g right0=%g right1=%g right2=%g right3=%g",
                             pNode->ID(),
                             NodeDebugLabel(pNode).c_str(),
                             resolution,
                             m_scene->scriptHost != nullptr ? "scriptHost"
                             : m_scene->soundManager != nullptr ? "soundManager"
                                                                : "none",
                             info.has_audio_spectrum_left[index] ? "true" : "false",
                             info.has_audio_spectrum_right[index] ? "true" : "false",
                             left.size(),
                             right.size(),
                             left.size() > 0 ? left[0] : 0.0f,
                             left.size() > 1 ? left[1] : 0.0f,
                             left.size() > 2 ? left[2] : 0.0f,
                             left.size() > 3 ? left[3] : 0.0f,
                             right.size() > 0 ? right[0] : 0.0f,
                             right.size() > 1 ? right[1] : 0.0f,
                             right.size() > 2 ? right[2] : 0.0f,
                             right.size() > 3 ? right[3] : 0.0f);
                }

                if (info.has_audio_spectrum_left[index]) {
                    updateOp(kAudioSpectrumLeftUniforms[index],
                             std::span<const float> { left.data(), left.size() });
                }
                if (info.has_audio_spectrum_right[index]) {
                    updateOp(kAudioSpectrumRightUniforms[index],
                             std::span<const float> { right.data(), right.size() });
                }
            }
        }
    }

    if (m_scene->scriptHost) {
        m_scene->scriptHost->ApplyTextureAnimations(pNode, sprites, m_scene->frameTime);
    }

    for (auto& [i, sp] : sprites) {
        const auto& f      = sp.GetAnimateFrame(m_scene->frameTime);
        auto        grot   = WE_GLTEX_ROTATION_NAMES[i];
        auto        gtrans = WE_GLTEX_TRANSLATION_NAMES[i];
        updateOp(grot, std::array { f.xAxis[0], f.xAxis[1], f.yAxis[0], f.yAxis[1] });
        updateOp(gtrans, std::array { f.x, f.y });
    }

    if (info.has_LP || info.has_model_LCP || info.has_LCR) {
        std::array<float, 16> lights { 0 };
        std::array<float, 12> lights_color { 0 };
        std::array<float, 16> lights_color_radius { 0 };
        uint                  i = 0;
        for (auto& l : m_scene->lights) {
            if (i == 4) break;
            assert(l->node() != nullptr);
            l->node()->UpdateTrans();
            const auto modelTrans = l->node()->ModelTrans();
            lights[i * 4 + 0]     = (float)modelTrans(0, 3);
            lights[i * 4 + 1]     = (float)modelTrans(1, 3);
            lights[i * 4 + 2]     = (float)modelTrans(2, 3);
            // g_LightsColorRadius is distinct from g_LightsColorPremultiplied: Demon Core's
            // core.frag feeds rgb directly into ComputeLightSpecular and keeps the falloff radius in
            // w. Sending the radius-squared premultiplied payload here overdrives the sphere into a
            // clipped red/white blob, while color*intensity matches the shader's authored contract.
            const auto color_radius = l->colorIntensity();
            lights_color_radius[i * 4 + 0] = color_radius[0];
            lights_color_radius[i * 4 + 1] = color_radius[1];
            lights_color_radius[i * 4 + 2] = color_radius[2];
            lights_color_radius[i * 4 + 3] = l->radius();
            if (i < 3) {
                const auto& color = l->premultipliedColor();
                std::copy(color.begin(), color.end(), lights_color.begin() + i * 4);
            }
            i++;
        }
        if (info.has_LP) updateOp(G_LP, lights);
        if (info.has_LP || info.has_model_LCP) updateOp(G_LCP, lights_color);
        if (info.has_LCR) updateOp(G_LCR, lights_color_radius);
    }
}

void WPShaderValueUpdater::SetNodeData(void* nodeAddr, const WPShaderValueData& data) {
    m_nodeDataMap[nodeAddr] = data;
}

void WPShaderValueUpdater::ReplaceNodeReferences(SceneNode* old_node, SceneNode* new_node) {
    if (old_node == nullptr || new_node == nullptr || old_node == new_node) return;

    auto old_data_it = m_nodeDataMap.find(old_node);
    auto new_data_it = m_nodeDataMap.find(new_node);
    if (old_data_it != m_nodeDataMap.end() && new_data_it != m_nodeDataMap.end()) {
        PreserveDeferredRuntimeParallaxContract(
            old_data_it->second, new_data_it->second, old_node, new_node);
    }

    for (auto& [_, data] : m_nodeDataMap) {
        (void)_;
        if (data.parallax_anchor == old_node) data.parallax_anchor = new_node;
        if (data.transform_binding.parent == old_node) data.transform_binding.parent = new_node;
        if (data.effect_projection_node == old_node) data.effect_projection_node = new_node;
    }

    // Deferred materialization destroys the hidden placeholder after the real layer node is built.
    // Any remaining shader-data/cache entry keyed by the placeholder address can later match a
    // recycled allocation and send transform resolution through freed memory, so remove every
    // per-frame structure that treats the raw SceneNode pointer as an identity key.
    m_nodeDataMap.erase(old_node);
    m_nodeUniformInfoMap.erase(old_node);
    m_modelTransformCache.clear();
    m_parallaxOffsetCache.clear();
    m_attachmentTransformCache.clear();
}

const WPShaderValueData* WPShaderValueUpdater::GetNodeData(const void* node_addr) const {
    auto it = m_nodeDataMap.find(const_cast<void*>(node_addr));
    return it == m_nodeDataMap.end() ? nullptr : std::addressof(it->second);
}

WPShaderValueData* WPShaderValueUpdater::GetNodeData(const void* node_addr) {
    auto it = m_nodeDataMap.find(const_cast<void*>(node_addr));
    return it == m_nodeDataMap.end() ? nullptr : std::addressof(it->second);
}

void WPShaderValueUpdater::SetTexelSize(float x, float y) { m_texelSize = { x, y }; }
