#pragma once

#include "runtime/diagnostics/Diagnostics.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace wallpaper
{
class FrameTimer;
namespace audio
{
class SoundManager;
}
namespace fs
{
class Fs;
class VFS;
}
namespace looper
{
class Looper;
}

struct FileSystemService {
    std::function<bool(const std::filesystem::path&)> exists;
    std::function<bool(const std::filesystem::path&)> createDirectories;
    std::function<std::unique_ptr<fs::VFS>()> createVfs;
    std::function<std::unique_ptr<fs::Fs>(std::string_view, bool)> createPhysicalFs;
    std::function<std::unique_ptr<fs::Fs>(std::string_view)> createPackageFs;
};

struct AudioService {
    bool available { true };
    std::function<std::unique_ptr<audio::SoundManager>()> createSoundManager;
};

struct MediaService {
    bool available { false };
};

struct TimerService {
    std::function<std::uint64_t()> monotonicMilliseconds;
    std::function<std::shared_ptr<looper::Looper>()> createLooper;
    std::function<std::unique_ptr<FrameTimer>()> createFrameTimer;
};

struct PlatformService {
    std::function<std::filesystem::path(std::string_view)> cachePathForApp;
};

struct CacheService {
    std::function<std::filesystem::path(std::string_view)> resolveCacheRoot;
};

struct DiagnosticsService {
    std::function<void(DiagnosticSeverity, std::string_view, std::string_view)> publish;
};

struct HostServices {
    FileSystemService  fileSystem;
    AudioService       audio;
    MediaService       media;
    TimerService       timer;
    PlatformService    platform;
    CacheService       cache;
    DiagnosticsService diagnostics;
};

std::shared_ptr<HostServices> CreateDefaultHostServices();
} // namespace wallpaper
