#pragma once

#include <functional>
#include <string_view>

namespace wallpaper
{
using FirstFrameCallback = std::function<void()>;

constexpr std::string_view WE_SCENE_PROPERTY_SOURCE                    = "source";
constexpr std::string_view WE_SCENE_PROPERTY_ASSETS                    = "assets";
constexpr std::string_view WE_SCENE_PROPERTY_FPS                       = "fps";
constexpr std::string_view WE_SCENE_PROPERTY_FILLMODE                  = "fillmode";
constexpr std::string_view WE_SCENE_PROPERTY_SPEED                     = "speed";
constexpr std::string_view WE_SCENE_PROPERTY_GRAPHVIZ                  = "graphviz";
constexpr std::string_view WE_SCENE_PROPERTY_GRAPHVIZ_PATH             = "graphviz_path";
constexpr std::string_view WE_SCENE_PROPERTY_GRAPHIVZ                  = "graphivz";
constexpr std::string_view WE_SCENE_PROPERTY_VOLUME                    = "volume";
constexpr std::string_view WE_SCENE_PROPERTY_MUTED                     = "muted";
constexpr std::string_view WE_SCENE_PROPERTY_CACHE_PATH                = "cache_path";
constexpr std::string_view WE_SCENE_PROPERTY_FIRST_FRAME_CALLBACK      = "first_frame_callback";
constexpr std::string_view WE_SCENE_PROPERTY_LOAD_USER_PROPERTIES      = "load_user_properties";
constexpr std::string_view WE_SCENE_PROPERTY_USER_PROPERTIES           = "user_properties";
constexpr std::string_view WE_SCENE_PROPERTY_LOAD_USER_PROPERTIES_JSON = "load_user_properties_json";
constexpr std::string_view WE_SCENE_PROPERTY_USER_PROPERTIES_JSON      = "user_properties_json";
constexpr std::string_view WE_SCENE_PROPERTY_AUDIO_SAMPLES             = "audio_samples";
constexpr std::string_view WE_SCENE_PROPERTY_CAPTURE_FRAME             = "capture_frame";
constexpr std::string_view WE_SCENE_PROPERTY_CAPTURE_FRAME_NUMBER      = "capture_frame_number";
} // namespace wallpaper
