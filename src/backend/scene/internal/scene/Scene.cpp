#include "Scene.h"

#include "fs/VFS.h"
#include "interface/IImageParser.h"
#include "interface/IShaderValueUpdater.h"
#include "particle/ParticleSystem.h"

#include <algorithm>
#include <unordered_set>

namespace wallpaper 
{
namespace
{
bool IsLayerVisibleImpl(const Scene& scene, int32_t layer_id, std::unordered_set<int32_t>& visiting) {
    if (layer_id == 0) return true;
    if (! visiting.insert(layer_id).second) return true;

    const auto visible_it = scene.layerLocalVisibility.find(layer_id);
    const bool local_visible =
        visible_it == scene.layerLocalVisibility.end() ? true : visible_it->second;
    if (! local_visible) return false;

    const auto binding_it = scene.layerParentBindings.find(layer_id);
    if (binding_it == scene.layerParentBindings.end() || binding_it->second.parent_id == 0) {
        return true;
    }

    return IsLayerVisibleImpl(scene, binding_it->second.parent_id, visiting);
}

void ApplyLayerVisibilityRecursive(Scene& scene, int32_t layer_id, std::unordered_set<int32_t>& visited) {
    if (layer_id == 0 || ! visited.insert(layer_id).second) return;

    std::unordered_set<int32_t> visiting;
    const bool effective_visible = IsLayerVisibleImpl(scene, layer_id, visiting);

    if (const auto runtime_nodes_it = scene.objectRuntimeNodes.find(layer_id);
        runtime_nodes_it != scene.objectRuntimeNodes.end()) {
        for (auto* node : runtime_nodes_it->second) {
            if (node != nullptr) node->SetLayerVisible(effective_visible);
        }
    }

    if (const auto layer_node_it = scene.layerNodes.find(layer_id);
        layer_node_it != scene.layerNodes.end() && layer_node_it->second != nullptr) {
        layer_node_it->second->SetLayerVisible(effective_visible);
    }

    for (const auto& [child_id, binding] : scene.layerParentBindings) {
        if (binding.parent_id == layer_id) {
            ApplyLayerVisibilityRecursive(scene, child_id, visited);
        }
    }
}
} // namespace

Scene::Scene(): sceneGraph(std::make_shared<SceneNode>()) ,paritileSys(std::make_unique<ParticleSystem>(*this)) {}
Scene::~Scene() = default;

int32_t Scene::AllocateLayerId() const {
    int32_t layer_id = 1;
    for (const auto existing_id : layerOrder) {
        layer_id = std::max(layer_id, existing_id + 1);
    }
    for (const auto& [existing_id, _] : layerNodes) {
        layer_id = std::max(layer_id, existing_id + 1);
    }
    return layer_id;
}

bool Scene::RegisterLayer(int32_t layer_id,
                          std::string name,
                          SceneNode* node,
                          std::string initial_config_json) {
    if (layer_id <= 0) {
        return false;
    }

    if (std::find(layerOrder.begin(), layerOrder.end(), layer_id) == layerOrder.end()) {
        layerOrder.push_back(layer_id);
    }

    layerNodes[layer_id] = node;
    if (layerLocalVisibility.find(layer_id) == layerLocalVisibility.end()) {
        layerLocalVisibility[layer_id] = true;
    }
    if (node != nullptr && !name.empty()) {
        node->SetName(name);
    }

    for (auto it = layerNameToId.begin(); it != layerNameToId.end();) {
        if (it->second == layer_id || (!name.empty() && it->first == name)) {
            it = layerNameToId.erase(it);
        } else {
            ++it;
        }
    }
    if (!name.empty()) {
        layerNameToId[std::move(name)] = layer_id;
    }
    if (!initial_config_json.empty()) {
        initialLayerConfigJson[layer_id] = std::move(initial_config_json);
    }
    return true;
}

bool Scene::CreateRuntimeLayer(std::string name,
                               std::string initial_config_json,
                               int32_t* out_layer_id) {
    const int32_t layer_id = AllocateLayerId();
    auto node = std::make_shared<SceneNode>();
    node->ID() = layer_id;
    node->SetName(name);
    SceneNode* raw_node = node.get();
    sceneGraph->AppendChild(std::move(node));
    if (!RegisterLayer(layer_id, std::move(name), raw_node, std::move(initial_config_json))) {
        return false;
    }
    SetLayerLocalVisibility(layer_id, true);
    ApplyLayerVisibility(layer_id);
    MarkRenderGraphTopologyDirty();
    if (out_layer_id != nullptr) {
        *out_layer_id = layer_id;
    }
    return true;
}

bool Scene::DestroyLayer(int32_t layer_id) {
    if (layerNodes.erase(layer_id) == 0 &&
        std::find(layerOrder.begin(), layerOrder.end(), layer_id) == layerOrder.end()) {
        return false;
    }

    layerOrder.erase(std::remove(layerOrder.begin(), layerOrder.end(), layer_id), layerOrder.end());
    initialLayerConfigJson.erase(layer_id);
    layerParentBindings.erase(layer_id);
    layerLocalVisibility.erase(layer_id);
    textLayers.erase(layer_id);
    textPrimitives.erase(layer_id);
    dirtyTextLayerIds.erase(layer_id);
    runtimeParticleSubsystemsByObjectId.erase(layer_id);
    for (auto it = runtimeParticleObjectIdsByName.begin(); it != runtimeParticleObjectIdsByName.end();) {
        if (it->second == layer_id) {
            it = runtimeParticleObjectIdsByName.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = layerNameToId.begin(); it != layerNameToId.end();) {
        if (it->second == layer_id) {
            it = layerNameToId.erase(it);
        } else {
            ++it;
        }
    }
    MarkRenderGraphTopologyDirty();
    return true;
}

bool Scene::SortLayer(int32_t layer_id, int32_t target_index) {
    auto it = std::find(layerOrder.begin(), layerOrder.end(), layer_id);
    if (it == layerOrder.end()) {
        return false;
    }

    layerOrder.erase(it);
    target_index = std::clamp(target_index, 0, static_cast<int32_t>(layerOrder.size()));
    layerOrder.insert(layerOrder.begin() + target_index, layer_id);
    MarkRenderGraphTopologyDirty();
    return true;
}

void Scene::SetLayerParentBinding(int32_t layer_id,
                                  int32_t parent_id,
                                  std::string attachment) {
    if (layer_id == 0) return;
    if (parent_id == 0 && attachment.empty()) {
        layerParentBindings.erase(layer_id);
        ApplyLayerVisibility(layer_id);
        return;
    }
    layerParentBindings[layer_id] = LayerParentBinding {
        .parent_id = parent_id,
        .attachment = std::move(attachment),
    };
    ApplyLayerVisibility(layer_id);
}

Scene::LayerParentBinding Scene::GetLayerParentBinding(int32_t layer_id) const {
    const auto it = layerParentBindings.find(layer_id);
    return it == layerParentBindings.end() ? LayerParentBinding {} : it->second;
}

void Scene::ClearLayerParentBinding(int32_t layer_id) {
    layerParentBindings.erase(layer_id);
    ApplyLayerVisibility(layer_id);
}

int32_t Scene::ResolveLayer(std::string_view name) const {
    const auto it = layerNameToId.find(std::string(name));
    return it == layerNameToId.end() ? 0 : it->second;
}

int32_t Scene::LayerIndex(int32_t layer_id) const {
    const auto it = std::find(layerOrder.begin(), layerOrder.end(), layer_id);
    if (it == layerOrder.end()) {
        return -1;
    }
    return static_cast<int32_t>(std::distance(layerOrder.begin(), it));
}

void Scene::SetLayerLocalVisibility(int32_t layer_id, bool visible) {
    if (layer_id == 0) return;
    layerLocalVisibility[layer_id] = visible;
}

bool Scene::GetLayerLocalVisibility(int32_t layer_id) const {
    const auto it = layerLocalVisibility.find(layer_id);
    return it == layerLocalVisibility.end() ? true : it->second;
}

bool Scene::IsLayerVisible(int32_t layer_id) const {
    std::unordered_set<int32_t> visiting;
    return IsLayerVisibleImpl(*this, layer_id, visiting);
}

void Scene::ApplyLayerVisibility(int32_t layer_id) {
    std::unordered_set<int32_t> visited;
    ApplyLayerVisibilityRecursive(*this, layer_id, visited);
}

void Scene::ApplyAllLayerVisibility() {
    std::unordered_set<int32_t> visited;
    for (const auto layer_id : layerOrder) {
        ApplyLayerVisibilityRecursive(*this, layer_id, visited);
    }
    for (const auto& [layer_id, _] : layerNodes) {
        ApplyLayerVisibilityRecursive(*this, layer_id, visited);
    }
}

}
