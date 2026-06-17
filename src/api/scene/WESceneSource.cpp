#include "api/scene/WESceneSource.hpp"

namespace wallpaper
{
WallpaperSource MakeWESceneWallpaperSource(const WESceneSourceConfig& config) {
    WallpaperSource source;
    source.type = BackendType::WEScene;
    source.uri  = config.uri;

    if (! config.assets.empty()) {
        source.initialProperties.emplace(std::string(WE_SCENE_PROPERTY_ASSETS), config.assets);
    }
    source.initialProperties.emplace(std::string(WE_SCENE_PROPERTY_FPS), config.fps);
    source.initialProperties.emplace(std::string(WE_SCENE_PROPERTY_FILLMODE), config.fillMode);
    source.initialProperties.emplace(std::string(WE_SCENE_PROPERTY_SPEED), config.speed);
    source.initialProperties.emplace(std::string(WE_SCENE_PROPERTY_VOLUME), config.volume);
    source.initialProperties.emplace(std::string(WE_SCENE_PROPERTY_MUTED), config.muted);
    source.initialProperties.emplace(std::string(WE_SCENE_PROPERTY_GRAPHIVZ), config.graphviz);
    return source;
}
} // namespace wallpaper
