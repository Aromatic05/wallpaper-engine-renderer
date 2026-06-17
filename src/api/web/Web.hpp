#pragma once

#include "api/WallpaperSession.hpp"

#include <string>

namespace wallpaper
{
struct WebSourceConfig {
    std::string uri;
};

inline WallpaperSource MakeWebWallpaperSource(const WebSourceConfig& config) {
    WallpaperSource source;
    source.type = BackendType::Web;
    source.uri  = config.uri;
    return source;
}

inline Result<void> LoadWebWallpaper(WallpaperSession& session, const WebSourceConfig& sourceConfig) {
    return session.load(MakeWebWallpaperSource(sourceConfig));
}
} // namespace wallpaper
