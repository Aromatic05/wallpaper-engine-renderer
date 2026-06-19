#include "Scene.h"

#include "fs/VFS.h"
#include "interface/IImageParser.h"
#include "interface/IShaderValueUpdater.h"
#include "particle/ParticleSystem.h"

#include <algorithm>

namespace wallpaper 
{

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

}

