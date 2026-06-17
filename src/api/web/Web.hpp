#pragma once

#include "../WallpaperRuntime.hpp"
#include "../WallpaperSession.hpp"

#include <string>

namespace wallpaper
{
struct WebSourceConfig {
    std::string uri;
};

std::unique_ptr<WallpaperSession> CreateWebSession(WallpaperRuntime& runtime,
                                                   std::string       cachePath = {});

WallpaperSource MakeWebWallpaperSource(const WebSourceConfig& config);
Result<void>    LoadWebWallpaper(WallpaperSession& session, const WebSourceConfig& sourceConfig);
} // namespace wallpaper
