#pragma once

#include "../runtime/property/PropertyValue.hpp"

#include <memory>
#include <string>

namespace wallpaper
{
class BackendFactory;
struct HostServices;

enum class BackendType
{
    WEScene,
    Web,
    Image,
    Video
};

struct WallpaperSource {
    BackendType type;
    std::string uri;
    PropertyMap initialProperties;
};

struct SessionConfig {
    std::shared_ptr<BackendFactory> backendFactory;
    std::shared_ptr<HostServices>   hostServices;
    std::string                     cachePath;
};
} // namespace wallpaper
