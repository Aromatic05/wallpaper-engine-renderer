#pragma once

#include "WallpaperTypes.hpp"

#include <memory>

namespace wallpaper
{
class WallpaperSession;

class WallpaperRuntime {
public:
    std::unique_ptr<WallpaperSession> createSession(const SessionConfig& config);
};
} // namespace wallpaper
