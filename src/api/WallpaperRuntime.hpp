#pragma once

#include "api/WallpaperSession.hpp"

#include <memory>

namespace wallpaper
{
class WallpaperRuntime {
public:
    std::unique_ptr<WallpaperSession> createSession(const SessionConfig& config);
};
} // namespace wallpaper
