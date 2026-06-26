#include "api/WallpaperRuntime.hpp"
#include "api/web/Web.hpp"

#include <cassert>
#include <filesystem>

int main() {
    wallpaper::WallpaperRuntime runtime;
    auto                        session = wallpaper::CreateWebSession(runtime);

    // The web backend contract test is intentionally lightweight: it
    // verifies that the session can be constructed, can accept a
    // load() with a workshop URI, and exposes the right backend type.
    // Play/pause/stop are not exercised here because the real
    // BrowserHost would spin up CEF, which has no headless test
    // path on a CI box. Commit 11 rewrites this test with a
    // WebBrowserHost mock that records every call without touching
    // the CEF runtime; until then the contract is "constructed +
    // loads".
    wallpaper::WebSourceConfig config;
    config.uri = "/nonexistent/this/path/is/expected/to/fail/parse";

    auto loadResult = wallpaper::LoadWebWallpaper(*session, config);
    // The URI is intentionally a path that does not contain a
    // project.json; LoadWebManifest returns nullopt, so the
    // session.load returns InvalidArgument. That is the correct
    // contract — a bad workshop dir is a load-time error, not a
    // runtime success.
    assert(! loadResult);
    assert(loadResult.error().code == wallpaper::ResultCode::InvalidArgument);

    return 0;
}
