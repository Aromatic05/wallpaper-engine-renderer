#pragma once

#include "runtime/session/WallpaperSession.hpp"

#include <memory>

namespace wallpaper
{
class WallpaperRuntime;

SessionConfig MakeBuiltinSessionConfig(std::string cachePath = {});

std::unique_ptr<WallpaperSession> CreateBuiltinSession(WallpaperRuntime& runtime,
                                                       std::string       cachePath = {});

class WallpaperRuntime {
public:
    std::unique_ptr<WallpaperSession> createSession(const SessionConfig& config);
};
} // namespace wallpaper
