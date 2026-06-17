#pragma once

#include "runtime/property/PropertyValue.hpp"

#include <memory>
#include <string>

namespace wallpaper
{
class BackendFactory;

enum class BackendType
{
    WEScene,
    Web,
    Image,
    Video
};

struct WallpaperSource {
    BackendType type { BackendType::WEScene };
    std::string uri;
    PropertyMap initialProperties;
};

struct SessionConfig {
    std::shared_ptr<BackendFactory> backendFactory;
};
} // namespace wallpaper
