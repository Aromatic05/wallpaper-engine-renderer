#include "api/web/Web.hpp"
#include "backend/BuiltinSessionFactory.hpp"

#include <utility>
namespace wallpaper
{
std::unique_ptr<WallpaperSession> CreateWebSession(WallpaperRuntime& runtime, std::string cachePath) {
    return CreateBuiltinSession(runtime, std::move(cachePath));
}

WallpaperSource MakeWebWallpaperSource(const WebSourceConfig& config) {
    WallpaperSource source;
    source.type = BackendType::Web;
    source.uri  = config.uri;
    return source;
}

Result<void> LoadWebWallpaper(WallpaperSession& session, const WebSourceConfig& sourceConfig) {
    return session.load(MakeWebWallpaperSource(sourceConfig));
}
} // namespace wallpaper
