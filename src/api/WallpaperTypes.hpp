#pragma once

#include "output/OutputTarget.hpp"
#include "runtime/diagnostics/Diagnostics.hpp"
#include "runtime/input/InputEvent.hpp"
#include "runtime/property/PropertyValue.hpp"
#include "runtime/session/SessionState.hpp"

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
    BackendType type;
    std::string uri;
    PropertyMap initialProperties;
};

struct SessionConfig {
    std::shared_ptr<BackendFactory> backendFactory;
    std::string                     cachePath;
};
} // namespace wallpaper
