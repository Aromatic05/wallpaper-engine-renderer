#include "api/WallpaperRuntime.hpp"

#include "api/WallpaperSession.hpp"

namespace wallpaper
{
std::unique_ptr<WallpaperSession> WallpaperRuntime::createSession(const SessionConfig& config) {
    return std::make_unique<WallpaperSession>(config);
}
} // namespace wallpaper
