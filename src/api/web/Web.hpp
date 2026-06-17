#pragma once

#include "api/WallpaperSession.hpp"
#include "backend/web/WebBackend.hpp"

namespace wallpaper
{
inline Result<void> LoadWebWallpaper(WallpaperSession& session, const WebSourceConfig& sourceConfig) {
    return session.load(MakeWebWallpaperSource(sourceConfig));
}
} // namespace wallpaper
