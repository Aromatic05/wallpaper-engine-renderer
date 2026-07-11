#include "WPSceneLayerMetadata.hpp"

#include <limits>

namespace wallpaper
{
bool SceneLayerDisablesParallaxPropagation(const nlohmann::json& object) {
    if (! object.is_object()) return false;
    if (! object.contains("image") || object.at("image").is_null()) return false;
    const auto it = object.find("disablepropagation");
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

std::unordered_set<int32_t> CollectParallaxPropagationDisabledLayerIds(
    const nlohmann::json& sceneJson) {
    std::unordered_set<int32_t> result;
    const auto objectsIt = sceneJson.find("objects");
    if (objectsIt == sceneJson.end() || ! objectsIt->is_array()) return result;

    for (const auto& object : *objectsIt) {
        if (! SceneLayerDisablesParallaxPropagation(object)) continue;
        const auto idIt = object.find("id");
        if (idIt == object.end() || ! idIt->is_number_integer()) continue;
        const auto id = idIt->get<int64_t>();
        if (id <= 0 || id > std::numeric_limits<int32_t>::max()) continue;
        result.insert(static_cast<int32_t>(id));
    }
    return result;
}
} // namespace wallpaper
