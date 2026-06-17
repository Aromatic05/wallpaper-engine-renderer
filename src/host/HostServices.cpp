#include "host/HostServices.hpp"

#include "host/audio/include/audio/SoundManager.h"
#include "host/looper/include/looper/Looper.hpp"
#include "utils/Platform.hpp"

#include <chrono>

namespace wallpaper
{
std::shared_ptr<HostServices> CreateDefaultHostServices() {
    auto services = std::make_shared<HostServices>();

    services->fileSystem.exists = [](const std::filesystem::path& path) {
        return std::filesystem::exists(path);
    };
    services->fileSystem.createDirectories = [](const std::filesystem::path& path) {
        std::error_code error;
        const bool      created = std::filesystem::create_directories(path, error);
        return ! error && (created || std::filesystem::exists(path));
    };

    services->timer.monotonicMilliseconds = []() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    };
    services->timer.createLooper = []() {
        return std::make_shared<looper::Looper>();
    };

    services->audio.createSoundManager = []() {
        return std::make_unique<audio::SoundManager>();
    };

    services->platform.cachePathForApp = [](std::string_view appName) {
        return platform::GetCachePath(appName);
    };

    services->cache.resolveCacheRoot = [services](std::string_view appName) {
        if (! services->platform.cachePathForApp) {
            return std::filesystem::path {};
        }
        return services->platform.cachePathForApp(appName);
    };

    services->diagnostics.publish = [](DiagnosticSeverity, std::string_view, std::string_view) {};

    return services;
}
} // namespace wallpaper
