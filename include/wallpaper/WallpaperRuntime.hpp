#pragma once

#include "FrameLifecycle.hpp"
#include "Result.hpp"
#include "WallpaperTypes.hpp"

#include <memory>

namespace wallpaper
{
class WallpaperSession;

class WallpaperRuntime {
public:
    std::unique_ptr<WallpaperSession> createSession(const SessionConfig& config);
    Result<FrameLifecycle>            tick(WallpaperSession& session) const;
};
} // namespace wallpaper
