#pragma once

#include "WallpaperTypes.hpp"
#include "../common/result/Result.hpp"
#include "../runtime/session/FrameLifecycle.hpp"

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
