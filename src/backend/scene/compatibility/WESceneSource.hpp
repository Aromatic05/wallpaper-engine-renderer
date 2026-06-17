#pragma once

#include "runtime/session/WallpaperTypes.hpp"

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
    std::string uri;
    std::string assets;
    std::int32_t fps { 15 };
    std::int32_t fillMode { 0 };
    float speed { 1.0f };
    float volume { 1.0f };
    bool  muted { false };
    bool  graphviz { false };
};

WallpaperSource MakeWESceneWallpaperSource(const WESceneSourceConfig& config);
} // namespace wallpaper
