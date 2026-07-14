#include "abi/WeRendererOptions.hpp"
#include "wallpaper/scene/WESceneContract.hpp"

#include <cassert>
#include <string>

int main() {
    wallpaper::WallpaperSource source;
    source.type = wallpaper::BackendType::WEScene;

    auto result = wallpaper::ApplyRendererSourceOptionsJson(R"({
        "version": 1,
        "scene": {
            "audio": {
                "forceLoop": true
            },
            "userProperties": {
                "enabled": true,
                "scale": 1.25,
                "title": "clock",
                "tint": [0.1, 0.2, 0.3]
            },
            "graphviz": {
                "enabled": true,
                "path": "/tmp/render-graph.dot"
            }
        }
    })", source);
    assert(result);

    const auto forceAudioLoopIt = source.initialProperties.find(
        std::string(wallpaper::WE_SCENE_PROPERTY_FORCE_AUDIO_LOOP));
    assert(forceAudioLoopIt != source.initialProperties.end());
    assert(std::get<bool>(forceAudioLoopIt->second));

    const auto userIt = source.initialProperties.find(
        std::string(wallpaper::WE_SCENE_PROPERTY_LOAD_USER_PROPERTIES_JSON));
    assert(userIt != source.initialProperties.end());
    const auto* userJson = std::get_if<std::string>(&userIt->second);
    assert(userJson != nullptr);
    assert(userJson->find("\"enabled\":true") != std::string::npos);
    assert(userJson->find("\"tint\":[0.1,0.2,0.3]") != std::string::npos);

    const auto graphvizIt =
        source.initialProperties.find(std::string(wallpaper::WE_SCENE_PROPERTY_GRAPHVIZ));
    assert(graphvizIt != source.initialProperties.end());
    assert(std::get<bool>(graphvizIt->second));

    const auto graphvizPathIt =
        source.initialProperties.find(std::string(wallpaper::WE_SCENE_PROPERTY_GRAPHVIZ_PATH));
    assert(graphvizPathIt != source.initialProperties.end());
    assert(std::get<std::string>(graphvizPathIt->second) == "/tmp/render-graph.dot");

    auto normalized = wallpaper::NormalizeUserPropertiesJson(
        R"({"enabled":false,"value":2,"name":"x","color":[1,0.5,0]})");
    assert(normalized);
    assert(normalized.value().find("\"enabled\":false") != std::string::npos);

    assert(! wallpaper::NormalizeUserPropertiesJson(R"({"bad":null})"));
    assert(! wallpaper::NormalizeUserPropertiesJson(R"({"bad":[]})"));
    assert(! wallpaper::NormalizeUserPropertiesJson(R"({"bad":[1,"x"]})"));

    wallpaper::WallpaperSource invalidVersion;
    invalidVersion.type = wallpaper::BackendType::WEScene;
    assert(! wallpaper::ApplyRendererSourceOptionsJson(R"({"version":2})", invalidVersion));

    wallpaper::WallpaperSource invalidGraphviz;
    invalidGraphviz.type = wallpaper::BackendType::WEScene;
    assert(! wallpaper::ApplyRendererSourceOptionsJson(
        R"({"version":1,"scene":{"graphviz":{"enabled":"yes"}}})",
        invalidGraphviz));

    wallpaper::WallpaperSource invalidAudio;
    invalidAudio.type = wallpaper::BackendType::WEScene;
    assert(! wallpaper::ApplyRendererSourceOptionsJson(
        R"({"version":1,"scene":{"audio":true}})", invalidAudio));
    assert(! wallpaper::ApplyRendererSourceOptionsJson(
        R"({"version":1,"scene":{"audio":{"forceLoop":"yes"}}})", invalidAudio));

    wallpaper::WallpaperSource webSource;
    webSource.type = wallpaper::BackendType::Web;
    assert(wallpaper::ApplyRendererSourceOptionsJson(
        R"({"version":1,"scene":{"graphviz":{"enabled":true}}})", webSource));
    assert(webSource.initialProperties.empty());

    return 0;
}
