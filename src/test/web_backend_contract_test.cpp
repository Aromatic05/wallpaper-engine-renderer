#include "api/WallpaperRuntime.hpp"
#include "api/web/Web.hpp"

#include <cassert>

int main() {
    wallpaper::WallpaperRuntime runtime;
    auto                        session = wallpaper::CreateWebSession(runtime);

    wallpaper::WebSourceConfig config;
    config.uri = "https://example.invalid";

    auto loadResult = wallpaper::LoadWebWallpaper(*session, config);
    assert(loadResult);

    auto playResult = session->play();
    assert(! playResult);
    assert(playResult.error().code == wallpaper::ResultCode::NotSupported);
    assert(session->state() != wallpaper::SessionState::Playing);
    return 0;
}
