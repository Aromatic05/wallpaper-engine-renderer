#include "Scene.h"

#include "Image.hpp"
#include "SceneCamera.h"

#include "fs/VFS.h"
#include "interface/IImageParser.h"
#include "interface/IShaderValueUpdater.h"
#include "particle/ParticleSystem.h"
#include "scenescript/WPSceneScriptHost.hpp"
#include "utils/Logging.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <unordered_set>

namespace wallpaper 
{

namespace
{
std::size_t EstimateParsedImageBytes(const std::shared_ptr<Image>& image) {
    if (image == nullptr) return 0;

    std::size_t total = 0;
    for (const auto& slot : image->slots) {
        for (const auto& mipmap : slot.mipmaps) {
            if (mipmap.size > 0) {
                total += static_cast<std::size_t>(mipmap.size);
                continue;
            }
            total += static_cast<std::size_t>(std::max(mipmap.width, 0)) *
                     static_cast<std::size_t>(std::max(mipmap.height, 0)) * 4u;
        }
    }
    return total;
}

std::string FindDinoRunScoreLayerName(const Scene& scene, int32_t layer_id) {
    if (scene.scene_id.find("dino_run") == std::string::npos) return {};

    if (auto it = scene.textLayers.find(layer_id); it != scene.textLayers.end()) {
        if (it->second.object.name == "label_coins" || it->second.object.name == "label_top") {
            return it->second.object.name;
        }
    }

    if (auto it = scene.layerNodes.find(layer_id); it != scene.layerNodes.end() && it->second != nullptr) {
        const auto& name = it->second->Name();
        if (name == "label_coins" || name == "label_top") return name;
    }

    for (const auto& [name, id] : scene.layerNameToId) {
        if (id == layer_id && (name == "label_coins" || name == "label_top")) return name;
    }
    return {};
}

bool IsLayerVisibleImpl(const Scene& scene, int32_t layer_id, std::unordered_set<int32_t>& visiting) {
    if (layer_id == 0) return true;
    if (!visiting.insert(layer_id).second) return true;

    const auto visible_it = scene.layerLocalVisibility.find(layer_id);
    const bool local_visible =
        visible_it == scene.layerLocalVisibility.end() ? true : visible_it->second;
    if (!local_visible) return false;

    const auto binding_it = scene.layerParentBindings.find(layer_id);
    if (binding_it == scene.layerParentBindings.end() || binding_it->second.parent_id == 0) {
        return true;
    }

    return IsLayerVisibleImpl(scene, binding_it->second.parent_id, visiting);
}

Eigen::Vector3d ToVector3d(const std::array<float, 3>& value) {
    return Eigen::Vector3d(value[0], value[1], value[2]);
}

std::array<float, 3> LerpArray3(const std::array<float, 3>& lhs,
                                const std::array<float, 3>& rhs,
                                double ratio) {
    const auto t = static_cast<float>(std::clamp(ratio, 0.0, 1.0));
    return {
        lhs[0] + (rhs[0] - lhs[0]) * t,
        lhs[1] + (rhs[1] - lhs[1]) * t,
        lhs[2] + (rhs[2] - lhs[2]) * t,
    };
}

bool ResolveCameraPathSample(const Scene::CameraPathSegment& segment,
                             double local_time,
                             Scene::CameraPathKeyframe& out) {
    if (segment.keyframes.empty()) return false;
    if (segment.keyframes.size() == 1) {
        out = segment.keyframes.front();
        return true;
    }

    const auto clamped_time = std::clamp(local_time, 0.0, std::max(0.0, segment.duration));
    const auto& first = segment.keyframes.front();
    const auto& last = segment.keyframes.back();
    if (clamped_time <= first.timestamp) {
        out = first;
        return true;
    }
    if (clamped_time >= last.timestamp) {
        out = last;
        return true;
    }

    for (size_t index = 1; index < segment.keyframes.size(); index++) {
        const auto& lhs = segment.keyframes[index - 1];
        const auto& rhs = segment.keyframes[index];
        if (clamped_time > rhs.timestamp) continue;

        const auto span = rhs.timestamp - lhs.timestamp;
        const auto ratio = span > 1e-9 ? (clamped_time - lhs.timestamp) / span : 0.0;
        out.timestamp = clamped_time;
        out.eye = LerpArray3(lhs.eye, rhs.eye, ratio);
        out.center = LerpArray3(lhs.center, rhs.center, ratio);
        out.up = LerpArray3(lhs.up, rhs.up, ratio);
        return true;
    }

    out = last;
    return true;
}

void CollectLayerEffectNodes(const Scene& scene, int32_t layer_id, std::vector<SceneNode*>& nodes) {
    auto camera_names_it = scene.objectRuntimeCameraNames.find(layer_id);
    if (camera_names_it == scene.objectRuntimeCameraNames.end()) return;

    for (const auto& camera_name : camera_names_it->second) {
        auto camera_it = scene.cameras.find(camera_name);
        if (camera_it == scene.cameras.end() || !camera_it->second->HasImgEffect()) continue;

        auto* effect_layer = camera_it->second->GetImgEffect().get();
        if (effect_layer == nullptr) continue;

        if (effect_layer->HasFinalComposite()) {
            // Final composite nodes are owned by the image-effect bridge instead of the authored
            // scene tree, so they will not be reached by normal parent/child propagation. Treat
            // them as layer-owned runtime nodes here to keep layer visibility authoritative while
            // preserving effect-local visibility on the internal shader nodes.
            effect_layer->FinalNode().SetLocalVisible(effect_layer->ShouldRunFinalComposite());
            nodes.push_back(&effect_layer->FinalNode());
        }

        for (size_t effect_index = 0; effect_index < effect_layer->EffectCount(); effect_index++) {
            auto& effect = effect_layer->GetEffect(effect_index);
            for (auto& effect_node : effect->nodes) {
                if (effect_node.sceneNode) nodes.push_back(effect_node.sceneNode.get());
            }
        }
    }
}

void ApplyLayerVisibilityRecursive(Scene& scene, int32_t layer_id, std::unordered_set<int32_t>& visited) {
    if (layer_id == 0 || !visited.insert(layer_id).second) return;

    std::unordered_set<int32_t> visiting;
    const bool effective_visible = IsLayerVisibleImpl(scene, layer_id, visiting);
    const auto trace_name = FindDinoRunScoreLayerName(scene, layer_id);

    if (auto runtime_nodes_it = scene.objectRuntimeNodes.find(layer_id);
        runtime_nodes_it != scene.objectRuntimeNodes.end()) {
        for (auto* node : runtime_nodes_it->second) {
            if (node != nullptr) {
                // Layer visibility propagation must not overwrite a node's own local visibility
                // contract. Runtime-owned support nodes may intentionally stay hidden even while
                // their authored layer is visible, so the scene system only updates the
                // layer-level flag.
                node->SetLayerVisible(effective_visible);
            }
        }
    }

    std::vector<SceneNode*> effect_nodes;
    CollectLayerEffectNodes(scene, layer_id, effect_nodes);
    for (auto* node : effect_nodes) {
        if (node != nullptr) {
            // Effect nodes are also owned by the layer-visibility system, but they still need to
            // preserve any explicit local visibility decisions that the effect pipeline may make.
            node->SetLayerVisible(effective_visible);
        }
    }

    if (!trace_name.empty()) {
        const auto binding = scene.GetLayerParentBinding(layer_id);
        const auto runtime_nodes_it = scene.objectRuntimeNodes.find(layer_id);
        const size_t runtime_node_count =
            runtime_nodes_it == scene.objectRuntimeNodes.end() ? 0 : runtime_nodes_it->second.size();
    }

    for (const auto& [child_id, binding] : scene.layerParentBindings) {
        if (binding.parent_id == layer_id) {
            ApplyLayerVisibilityRecursive(scene, child_id, visited);
        }
    }
}

std::pair<int32_t, Scene::CameraLayerRuntimeState*> FindActiveCameraLayer(Scene& scene) {
    // Wallpaper Engine uses the bottom-most visible camera layer as the active view. Scene JSON is
    // parsed in layer order, so walking the recorded camera layer order backwards gives later
    // camera layers precedence while still letting user/script visibility changes disable them.
    for (auto it = scene.cameraLayerOrder.rbegin(); it != scene.cameraLayerOrder.rend(); ++it) {
        auto layer_it = scene.cameraLayers.find(*it);
        if (layer_it == scene.cameraLayers.end() || !layer_it->second.node) continue;
        if (!scene.IsLayerVisible(*it)) continue;
        return { *it, &layer_it->second };
    }
    return { 0, nullptr };
}

double SanitizeCameraZoom(double zoom, int32_t layer_id) {
    if (std::isfinite(zoom) && zoom > 0.0001) return zoom;

    // Invalid authored/user zoom values would collapse the orthographic projection to infinity.
    // Log the offending camera layer and keep a neutral zoom so the wallpaper remains visible.
    LOG_ERROR("SceneCameraLayer: invalid zoom %.6f on layer=%d, using 1.0", zoom, layer_id);
    return 1.0;
}

void ApplyCameraProjectionState(Scene& scene,
                                const std::string& camera_name,
                                SceneCamera& camera,
                                double zoom,
                                float fov,
                                int32_t layer_id) {
    if (camera.IsPerspective()) {
        camera.SetFov(fov);
    } else {
        const double safe_zoom = SanitizeCameraZoom(zoom, layer_id);
        camera.SetWidth(std::max(1.0, static_cast<double>(scene.ortho[0]) / safe_zoom));
        camera.SetHeight(std::max(1.0, static_cast<double>(scene.ortho[1]) / safe_zoom));
    }

    camera.Update();
    scene.UpdateLinkedCamera(camera_name);
}
} // namespace

Scene::Scene(): sceneGraph(std::make_shared<SceneNode>()) ,paritileSys(std::make_unique<ParticleSystem>(*this)) {}

Scene::~Scene() {
    ClearParsedImageCache();
}

std::shared_ptr<Image> Scene::CacheParsedImageResultLocked(
    const std::string& texture_key,
    std::shared_ptr<Image> image,
    std::chrono::steady_clock::time_point started_at,
    const char* success_event,
    const char* failure_event) {
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started_at)
                                .count();
    if (image != nullptr) {
        m_parsed_image_cache[texture_key] = image;
        LOG_VERBOSE("%s: key='%s' bytes=%zu duration=%.2fms",
                    success_event,
                    texture_key.c_str(),
                    EstimateParsedImageBytes(image),
                    static_cast<double>(elapsed_us) / 1000.0);
        return image;
    }

    m_failed_parsed_images.insert(texture_key);
    LOG_ERROR("%s: key='%s' duration=%.2fms",
              failure_event,
              texture_key.c_str(),
              static_cast<double>(elapsed_us) / 1000.0);
    return {};
}

std::shared_ptr<Image> Scene::GetParsedImageIfReady(const std::string& texture_key) {
    if (texture_key.empty()) return {};

    std::lock_guard lock(m_parsed_image_mutex);
    if (const auto cached_it = m_parsed_image_cache.find(texture_key);
        cached_it != m_parsed_image_cache.end()) {
        return cached_it->second;
    }
    if (m_failed_parsed_images.count(texture_key) != 0) return {};

    const auto pending_it = m_pending_parsed_images.find(texture_key);
    if (pending_it == m_pending_parsed_images.end()) return {};
    if (pending_it->second.future.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready) {
        return {};
    }

    const auto started_at = pending_it->second.started_at;
    auto       image      = pending_it->second.future.get();
    m_pending_parsed_images.erase(pending_it);

    return CacheParsedImageResultLocked(texture_key,
                                        std::move(image),
                                        started_at,
                                        "SceneImageAsyncParseComplete",
                                        "SceneImageAsyncParseFailed");
}

std::shared_ptr<Image> Scene::ParseImageBlockingCached(const std::string& texture_key) {
    if (texture_key.empty() || imageParser == nullptr) return {};

    {
        std::lock_guard lock(m_parsed_image_mutex);
        if (const auto cached_it = m_parsed_image_cache.find(texture_key);
            cached_it != m_parsed_image_cache.end()) {
            return cached_it->second;
        }
        if (m_failed_parsed_images.count(texture_key) != 0) return {};

        const auto pending_it = m_pending_parsed_images.find(texture_key);
        if (pending_it != m_pending_parsed_images.end()) {
            const auto started_at = pending_it->second.started_at;
            auto       image      = pending_it->second.future.get();
            m_pending_parsed_images.erase(pending_it);
            return CacheParsedImageResultLocked(texture_key,
                                                std::move(image),
                                                started_at,
                                                "SceneImageAsyncParseJoined",
                                                "SceneImageAsyncParseFailed");
        }
    }

    const auto started_at = std::chrono::steady_clock::now();
    auto       image      = imageParser->Parse(texture_key);

    std::lock_guard lock(m_parsed_image_mutex);
    return CacheParsedImageResultLocked(texture_key,
                                        std::move(image),
                                        started_at,
                                        "SceneImageParseBlocking",
                                        "SceneImageParseBlockingFailed");
}

Scene::ParsedImageRequest Scene::RequestParsedImageAsync(const std::string& texture_key) {
    if (texture_key.empty() || imageParser == nullptr) {
        return { ParsedImageRequestState::Failed, {} };
    }

    if (auto image = GetParsedImageIfReady(texture_key); image != nullptr) {
        return { ParsedImageRequestState::Ready, image };
    }

    {
        std::lock_guard lock(m_parsed_image_mutex);
        if (const auto cached_it = m_parsed_image_cache.find(texture_key);
            cached_it != m_parsed_image_cache.end()) {
            return { ParsedImageRequestState::Ready, cached_it->second };
        }
        if (m_failed_parsed_images.count(texture_key) != 0) {
            return { ParsedImageRequestState::Failed, {} };
        }
        if (m_pending_parsed_images.count(texture_key) != 0) {
            return { ParsedImageRequestState::Pending, {} };
        }

        auto*       parser   = imageParser.get();
        std::string key_copy = texture_key;
        PendingParsedImageRequest pending;
        pending.started_at = std::chrono::steady_clock::now();
        pending.future     = std::async(std::launch::async, [parser, key_copy]() {
            return parser != nullptr ? parser->Parse(key_copy) : std::shared_ptr<Image> {};
        });
        m_pending_parsed_images.emplace(texture_key, std::move(pending));
    }

    LOG_VERBOSE("SceneImageAsyncParseQueued: key='%s'", texture_key.c_str());
    return { ParsedImageRequestState::Pending, {} };
}

void Scene::DropParsedImageCache(std::string_view texture_key) {
    if (texture_key.empty()) return;

    const std::string key(texture_key);
    std::future<std::shared_ptr<Image>> pending_future;
    std::size_t dropped_bytes = 0;
    bool dropped_cached_image = false;
    bool dropped_pending_parse = false;
    {
        std::lock_guard lock(m_parsed_image_mutex);
        if (auto cached_it = m_parsed_image_cache.find(key);
            cached_it != m_parsed_image_cache.end()) {
            dropped_bytes = EstimateParsedImageBytes(cached_it->second);
            m_parsed_image_cache.erase(cached_it);
            dropped_cached_image = true;
        }
        if (auto pending_it = m_pending_parsed_images.find(key);
            pending_it != m_pending_parsed_images.end()) {
            pending_future = std::move(pending_it->second.future);
            m_pending_parsed_images.erase(pending_it);
            dropped_pending_parse = true;
        }
        m_failed_parsed_images.erase(key);
    }
    if (dropped_cached_image || dropped_pending_parse) {
        LOG_VERBOSE("SceneImageCacheDrop: key='%s' cached=%s bytes=%zu pending=%s",
                    key.c_str(),
                    dropped_cached_image ? "true" : "false",
                    dropped_bytes,
                    dropped_pending_parse ? "true" : "false");
    }
    if (pending_future.valid()) pending_future.wait();
}

void Scene::ClearParsedImageCache() {
    std::vector<std::future<std::shared_ptr<Image>>> pending_futures;
    {
        std::lock_guard lock(m_parsed_image_mutex);
        pending_futures.reserve(m_pending_parsed_images.size());
        for (auto& [_, request] : m_pending_parsed_images) {
            if (request.future.valid()) pending_futures.emplace_back(std::move(request.future));
        }
        m_parsed_image_cache.clear();
        m_pending_parsed_images.clear();
        m_failed_parsed_images.clear();
    }

    if (!pending_futures.empty()) {
        LOG_INFO("SceneImageAsyncParseJoin: pending=%zu", pending_futures.size());
        for (auto& future : pending_futures) {
            if (future.valid()) future.wait();
        }
    }
}

void Scene::SetLayerParentBinding(int32_t layer_id, int32_t parent_id, std::string attachment) {
    if (layer_id == 0) return;
    if (parent_id == 0 && attachment.empty()) {
        layerParentBindings.erase(layer_id);
        return;
    }
    layerParentBindings[layer_id] = LayerParentBinding {
        .parent_id = parent_id,
        .attachment = std::move(attachment),
    };
}

Scene::LayerParentBinding Scene::GetLayerParentBinding(int32_t layer_id) const {
    auto it = layerParentBindings.find(layer_id);
    return it == layerParentBindings.end() ? LayerParentBinding {} : it->second;
}

void Scene::ClearLayerParentBinding(int32_t layer_id) {
    layerParentBindings.erase(layer_id);
}

std::vector<int32_t> Scene::GetLayerChildren(int32_t layer_id) const {
    std::vector<int32_t> children;
    for (const auto& [child_id, binding] : layerParentBindings) {
        if (binding.parent_id == layer_id) children.push_back(child_id);
    }
    return children;
}

void Scene::SetLayerLocalVisibility(int32_t layer_id, bool visible) {
    if (layer_id == 0) return;

    layerLocalVisibility[layer_id] = visible;
}

bool Scene::GetLayerLocalVisibility(int32_t layer_id) const {
    auto it = layerLocalVisibility.find(layer_id);
    return it == layerLocalVisibility.end() ? true : it->second;
}

bool Scene::IsLayerVisible(int32_t layer_id) const {
    std::unordered_set<int32_t> visiting;
    return IsLayerVisibleImpl(*this, layer_id, visiting);
}

void Scene::ApplyLayerVisibility(int32_t layer_id) {
    std::unordered_set<int32_t> visited;
    ApplyLayerVisibilityRecursive(*this, layer_id, visited);
    if (!cameraLayers.empty()) UpdateActiveCameraLayer();
}

void Scene::ApplyAllLayerVisibility() {
    std::unordered_set<int32_t> visited;
    for (const auto layer_id : layerOrder) {
        ApplyLayerVisibilityRecursive(*this, layer_id, visited);
    }
    for (const auto& [layer_id, _] : layerNodes) {
        ApplyLayerVisibilityRecursive(*this, layer_id, visited);
    }
    if (!cameraLayers.empty()) UpdateActiveCameraLayer();
}

void Scene::UpdateModelCameraPath() {
    if (!modelCameraPathEnabled || modelCameraPathSegments.empty() ||
        modelPerspectiveCameraName.empty()) {
        return;
    }

    auto camera_it = cameras.find(modelPerspectiveCameraName);
    if (camera_it == cameras.end() || !camera_it->second) return;

    double total_duration = 0.0;
    for (const auto& segment : modelCameraPathSegments) {
        total_duration += std::max(0.0, segment.duration);
    }
    if (total_duration <= 1e-9) return;

    double path_time = std::fmod(std::max(0.0, elapsingTime), total_duration);
    if (path_time < 0.0) path_time += total_duration;

    int32_t active_segment = -1;
    double local_time = path_time;
    for (size_t index = 0; index < modelCameraPathSegments.size(); index++) {
        const auto duration = std::max(0.0, modelCameraPathSegments[index].duration);
        if (local_time <= duration || index + 1 == modelCameraPathSegments.size()) {
            active_segment = static_cast<int32_t>(index);
            break;
        }
        local_time -= duration;
    }
    if (active_segment < 0 ||
        active_segment >= static_cast<int32_t>(modelCameraPathSegments.size())) {
        return;
    }

    Scene::CameraPathKeyframe sample;
    if (!ResolveCameraPathSample(modelCameraPathSegments[active_segment], local_time, sample)) {
        return;
    }

    // Camera path playback is bound to the model-only camera name installed by WPModelObject
    // parsing. This deliberately avoids `global_perspective`, which is a legacy 2D particle camera.
    camera_it->second->SetExplicitView(ToVector3d(sample.eye),
                                       ToVector3d(sample.center),
                                       ToVector3d(sample.up));
    UpdateLinkedCamera(modelPerspectiveCameraName);

    if (activeModelCameraPathSegment != active_segment) {
        const auto& segment = modelCameraPathSegments[active_segment];
        LOG_INFO("Scene3DModelCameraPathActive: previous=%d active=%d duration=%.3f "
                 "local-time=%.3f eye=[%.3f, %.3f, %.3f] center=[%.3f, %.3f, %.3f]",
                 activeModelCameraPathSegment,
                 active_segment,
                 segment.duration,
                 local_time,
                 sample.eye[0],
                 sample.eye[1],
                 sample.eye[2],
                 sample.center[0],
                 sample.center[1],
                 sample.center[2]);
        activeModelCameraPathSegment = active_segment;
    }
}

Eigen::Vector3f Scene::ResolveCameraLayerNodeTranslation(
    const std::array<float, 3>& authored_origin) const {
    // WE 2D camera origins are authored around the static camera origin, where 0/0 means the
    // default centered wallpaper view. Hanabi's orthographic camera node is centered in render
    // coordinates, so add the canvas half-size before attaching the SceneCamera to this layer.
    return Eigen::Vector3f {
        static_cast<float>(ortho[0]) * 0.5f + authored_origin[0],
        static_cast<float>(ortho[1]) * 0.5f + authored_origin[1],
        authored_origin[2],
    };
}

void Scene::UpdateActiveCameraLayer() {
    auto [next_layer_id, camera_layer] = FindActiveCameraLayer(*this);

    std::string camera_name = "global";
    std::shared_ptr<SceneNode> camera_node = defaultGlobalCameraNode;
    double zoom = defaultGlobalCameraZoom;
    float fov = 50.0f;

    if (camera_layer != nullptr) {
        camera_name = camera_layer->camera_name.empty() ? "global" : camera_layer->camera_name;
        camera_node = camera_layer->node;
        zoom = camera_layer->zoom;
        fov = camera_layer->fov;
    }

    auto camera_it = cameras.find(camera_name);
    if (camera_it == cameras.end() || !camera_it->second) {
        LOG_ERROR("SceneCameraLayer: target camera '%s' for layer=%d is missing",
                  camera_name.c_str(),
                  next_layer_id);
        camera_it = cameras.find("global");
        camera_name = "global";
    }
    if (camera_it == cameras.end() || !camera_it->second || !camera_node) return;

    camera_it->second->AttatchNode(camera_node);
    ApplyCameraProjectionState(*this,
                               camera_name,
                               *camera_it->second,
                               zoom,
                               fov,
                               next_layer_id);
    activeCamera = camera_it->second.get();

    if (activeCameraLayerId != next_layer_id) {
        // This transition log is intentionally sparse: it proves which authored camera layer owns
        // the view without flooding frame logs while keyframed zoom/origin values animate.
        LOG_INFO("SceneCameraLayerActive: previous=%d active=%d camera='%s' zoom=%.3f origin=[%.3f, %.3f, %.3f]",
                 activeCameraLayerId,
                 next_layer_id,
                 camera_name.c_str(),
                 zoom,
                 camera_layer != nullptr ? camera_layer->origin[0] : 0.0f,
                 camera_layer != nullptr ? camera_layer->origin[1] : 0.0f,
                 camera_layer != nullptr ? camera_layer->origin[2] : 0.0f);
        activeCameraLayerId = next_layer_id;
    }
}

SceneImageEffect* Scene::FindImageEffect(int32_t owner_layer_id, uint32_t effect_index) {
    auto camera_names_it = objectRuntimeCameraNames.find(owner_layer_id);
    if (camera_names_it == objectRuntimeCameraNames.end()) return nullptr;

    for (const auto& camera_name : camera_names_it->second) {
        auto camera_it = cameras.find(camera_name);
        if (camera_it == cameras.end() || !camera_it->second->HasImgEffect()) continue;

        auto* effect_layer = camera_it->second->GetImgEffect().get();
        if (effect_layer == nullptr || effect_index >= effect_layer->EffectCount()) continue;
        return effect_layer->GetEffect(effect_index).get();
    }

    return nullptr;
}

const SceneImageEffect* Scene::FindImageEffect(int32_t owner_layer_id,
                                               uint32_t effect_index) const {
    return const_cast<Scene*>(this)->FindImageEffect(owner_layer_id, effect_index);
}

SceneImageEffect* Scene::FindImageEffectById(int32_t owner_layer_id, int32_t effect_id) {
    auto camera_names_it = objectRuntimeCameraNames.find(owner_layer_id);
    if (camera_names_it == objectRuntimeCameraNames.end()) return nullptr;

    for (const auto& camera_name : camera_names_it->second) {
        auto camera_it = cameras.find(camera_name);
        if (camera_it == cameras.end() || !camera_it->second->HasImgEffect()) continue;

        auto* effect_layer = camera_it->second->GetImgEffect().get();
        if (effect_layer == nullptr) continue;

        for (std::size_t effect_index = 0; effect_index < effect_layer->EffectCount();
             effect_index++) {
            auto& effect = effect_layer->GetEffect(effect_index);
            if (effect != nullptr && effect->EffectId() == effect_id) return effect.get();
        }
    }

    return nullptr;
}

const SceneImageEffect* Scene::FindImageEffectById(int32_t owner_layer_id,
                                                   int32_t effect_id) const {
    return const_cast<Scene*>(this)->FindImageEffectById(owner_layer_id, effect_id);
}

bool Scene::SetEffectLocalVisibility(int32_t owner_layer_id, uint32_t effect_index,
                                     bool visible) {
    auto* effect = FindImageEffect(owner_layer_id, effect_index);
    if (effect == nullptr) return false;

    // Only the effect-local bit changes here. The render graph topology remains valid because
    // hidden effects are handled by conditional execution and a bypass copy, while layer visibility
    // propagation still owns parent/child effective visibility.
    effect->SetLocalVisible(visible);
    ApplyLayerVisibility(owner_layer_id);
    return true;
}

bool Scene::SetEffectLocalVisibilityById(int32_t owner_layer_id, int32_t effect_id,
                                         bool visible) {
    auto* effect = FindImageEffectById(owner_layer_id, effect_id);
    if (effect == nullptr) return false;

    effect->SetLocalVisible(visible);
    ApplyLayerVisibility(owner_layer_id);
    return true;
}

}
