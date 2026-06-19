#pragma once

#include "../Result.hpp"
#include "../WallpaperRuntime.hpp"
#include "../WallpaperSession.hpp"
#include "WESceneContract.hpp"
#include "WESceneOutput.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace wallpaper
{
struct WESceneSourceConfig {
    std::string  uri;
    std::string  assets;
    std::int32_t fps { 15 };
    std::int32_t fillMode { 2 };
    float        speed { 1.0f };
    float        volume { 1.0f };
    bool         muted { false };
    bool         graphviz { false };
};

std::unique_ptr<WallpaperSession> CreateWESceneSession(WallpaperRuntime& runtime,
                                                       std::string       cachePath = {});

Result<void> LoadWEScene(WallpaperSession& session, const WESceneSourceConfig& sourceConfig);

Result<void> SetWESceneAssets(WallpaperSession& session, std::string assetsPath);
Result<void> SetWESceneFps(WallpaperSession& session, std::int32_t fps);
Result<void> SetWESceneFillMode(WallpaperSession& session, std::int32_t fillMode);
Result<void> SetWESceneSpeed(WallpaperSession& session, float speed);
Result<void> SetWESceneVolume(WallpaperSession& session, float volume);
Result<void> SetWESceneMuted(WallpaperSession& session, bool muted);
Result<void> SetWESceneGraphviz(WallpaperSession& session, bool enabled);
Result<void> SetWESceneFirstFrameCallback(WallpaperSession&                   session,
                                          std::shared_ptr<FirstFrameCallback> callback);

Result<std::shared_ptr<WESceneOutputBinding>> BindWESceneOutput(WallpaperSession&    session,
                                                                const RenderInitInfo& renderInitInfo);

Result<void> BindWESceneOutput(WallpaperSession&                           session,
                               const std::shared_ptr<WESceneOutputBinding>& binding);
} // namespace wallpaper
