#include "backend/scene/compatibility/WESceneSession.hpp"

#include "backend/BuiltinBackendFactory.hpp"

namespace wallpaper
{
SessionConfig MakeWESceneSessionConfig(std::string cachePath) {
    SessionConfig config;
    config.backendFactory = CreateBuiltinBackendFactory();
    config.cachePath      = std::move(cachePath);
    return config;
}

std::unique_ptr<WallpaperSession> CreateWESceneSession(WallpaperRuntime& runtime,
                                                       std::string       cachePath) {
    return runtime.createSession(MakeWESceneSessionConfig(std::move(cachePath)));
}

Result<void> LoadWEScene(WallpaperSession& session, const WESceneSourceConfig& sourceConfig) {
    return session.load(MakeWESceneWallpaperSource(sourceConfig));
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
