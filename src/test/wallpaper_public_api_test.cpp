#include <wallpaper/WallpaperRuntime.hpp>
#include <wallpaper/WallpaperSession.hpp>
#include <wallpaper/WallpaperTypes.hpp>
#include <wallpaper/scene/WEScene.hpp>
#include <wallpaper/scene/WESceneOutput.hpp>
#include <wallpaper/scene/WESceneSource.hpp>
#include <wallpaper/web/Web.hpp>

#include <memory>

int main() {
    wallpaper::SessionConfig   config;
    wallpaper::WallpaperSource source { wallpaper::BackendType::WEScene, "demo://scene", {} };

    auto runtime = std::make_unique<wallpaper::WallpaperRuntime>();
    auto session = runtime->createSession(config);
    (void)source;
    (void)session;
    return 0;
}
