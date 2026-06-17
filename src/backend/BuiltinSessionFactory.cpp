#include "backend/BuiltinSessionFactory.hpp"

#include "api/WallpaperSession.hpp"
#include "backend/BuiltinBackendFactory.hpp"
#include "host/HostServices.hpp"

namespace wallpaper
{
SessionConfig MakeBuiltinSessionConfig(std::string cachePath) {
    SessionConfig config;
    config.backendFactory = CreateBuiltinBackendFactory();
    config.hostServices   = CreateDefaultHostServices();
    config.cachePath      = std::move(cachePath);
    return config;
}

std::unique_ptr<WallpaperSession> CreateBuiltinSession(WallpaperRuntime& runtime,
                                                       std::string       cachePath) {
    return runtime.createSession(MakeBuiltinSessionConfig(std::move(cachePath)));
}
} // namespace wallpaper
