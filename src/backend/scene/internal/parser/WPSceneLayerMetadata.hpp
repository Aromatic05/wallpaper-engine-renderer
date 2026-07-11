#pragma once

#include <cstdint>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace wallpaper
{
bool SceneLayerDisablesParallaxPropagation(const nlohmann::json& object);
std::unordered_set<int32_t> CollectParallaxPropagationDisabledLayerIds(
    const nlohmann::json& sceneJson);
} // namespace wallpaper
