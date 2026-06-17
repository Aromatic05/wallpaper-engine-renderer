#include "api/scene/WEScene.hpp"
#include "api/scene/WESceneSource.hpp"

namespace wallpaper
{
std::unique_ptr<WallpaperSession> CreateWESceneSession(WallpaperRuntime& runtime,
                                                       std::string       cachePath) {
    return CreateBuiltinSession(runtime, std::move(cachePath));
}

Result<void> LoadWEScene(WallpaperSession& session, const WESceneSourceConfig& sourceConfig) {
    return session.load(MakeWESceneWallpaperSource(sourceConfig));
}

Result<void> SetWESceneAssets(WallpaperSession& session, std::string assetsPath) {
    return session.setProperty(WE_SCENE_PROPERTY_ASSETS, std::move(assetsPath));
}

Result<void> SetWESceneFps(WallpaperSession& session, std::int32_t fps) {
    return session.setProperty(WE_SCENE_PROPERTY_FPS, fps);
}

Result<void> SetWESceneFillMode(WallpaperSession& session, std::int32_t fillMode) {
    return session.setProperty(WE_SCENE_PROPERTY_FILLMODE, fillMode);
}

Result<void> SetWESceneSpeed(WallpaperSession& session, float speed) {
    return session.setProperty(WE_SCENE_PROPERTY_SPEED, speed);
}

Result<void> SetWESceneVolume(WallpaperSession& session, float volume) {
    return session.setProperty(WE_SCENE_PROPERTY_VOLUME, volume);
}

Result<void> SetWESceneMuted(WallpaperSession& session, bool muted) {
    return session.setProperty(WE_SCENE_PROPERTY_MUTED, muted);
}

Result<void> SetWESceneGraphviz(WallpaperSession& session, bool enabled) {
    return session.setProperty(WE_SCENE_PROPERTY_GRAPHIVZ, enabled);
}

Result<void> SetWESceneFirstFrameCallback(WallpaperSession&                   session,
                                          std::shared_ptr<FirstFrameCallback> callback) {
    return session.setProperty(WE_SCENE_PROPERTY_FIRST_FRAME_CALLBACK,
                               std::static_pointer_cast<void>(std::move(callback)));
}

Result<std::shared_ptr<WESceneOutputBinding>> BindWESceneOutput(WallpaperSession&    session,
                                                                const RenderInitInfo& renderInitInfo) {
    auto binding = MakeWESceneOutputBinding(renderInitInfo);
    auto result  = BindWESceneOutput(session, binding);
    if (! result) {
        return Result<std::shared_ptr<WESceneOutputBinding>>(result.error());
    }
    return Result<std::shared_ptr<WESceneOutputBinding>>::success(std::move(binding));
}

Result<void> BindWESceneOutput(WallpaperSession&                           session,
                               const std::shared_ptr<WESceneOutputBinding>& binding) {
    return session.bindOutput(MakeWESceneOutputTarget(binding));
}
} // namespace wallpaper
