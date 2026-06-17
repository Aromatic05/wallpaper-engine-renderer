#pragma once

#include "runtime/session/WallpaperSession.hpp"

#include <memory>

namespace wallpaper
{
class WallpaperRuntime;

class WallpaperRuntime {
public:
    std::unique_ptr<WallpaperSession> createSession(const SessionConfig& config);
};
} // namespace wallpaper
