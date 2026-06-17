#pragma once

#include "api/WallpaperRuntime.hpp"
#include "api/WallpaperSession.hpp"
#include "backend/scene/compatibility/WESceneOutputTarget.hpp"
#include "backend/scene/compatibility/WESceneRenderInit.hpp"
#include "common/result/Result.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace wallpaper
{
using FirstFrameCallback = std::function<void()>;

constexpr std::string_view WE_SCENE_PROPERTY_SOURCE               = "source";
constexpr std::string_view WE_SCENE_PROPERTY_ASSETS               = "assets";
constexpr std::string_view WE_SCENE_PROPERTY_FPS                  = "fps";
constexpr std::string_view WE_SCENE_PROPERTY_FILLMODE             = "fillmode";
constexpr std::string_view WE_SCENE_PROPERTY_SPEED                = "speed";
constexpr std::string_view WE_SCENE_PROPERTY_GRAPHIVZ             = "graphivz";
constexpr std::string_view WE_SCENE_PROPERTY_VOLUME               = "volume";
constexpr std::string_view WE_SCENE_PROPERTY_MUTED                = "muted";
constexpr std::string_view WE_SCENE_PROPERTY_CACHE_PATH           = "cache_path";
constexpr std::string_view WE_SCENE_PROPERTY_FIRST_FRAME_CALLBACK = "first_frame_callback";

struct WESceneSourceConfig {
    std::string  uri;
    std::string  assets;
    std::int32_t fps { 15 };
    std::int32_t fillMode { 0 };
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
