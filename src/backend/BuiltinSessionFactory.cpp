#include "backend/BuiltinSessionFactory.hpp"

#include "backend/BuiltinBackendFactory.hpp"

namespace wallpaper
{
SessionConfig MakeBuiltinSessionConfig(std::string cachePath) {
    SessionConfig config;
    config.backendFactory = CreateBuiltinBackendFactory();
    config.cachePath      = std::move(cachePath);
    return config;
}

std::unique_ptr<WallpaperSession> CreateBuiltinSession(WallpaperRuntime& runtime,
                                                       std::string       cachePath) {
    return runtime.createSession(MakeBuiltinSessionConfig(std::move(cachePath)));
}
} // namespace wallpaper
