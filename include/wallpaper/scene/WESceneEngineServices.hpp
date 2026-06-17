#pragma once

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

struct WESceneEngineServices {
    std::function<std::unique_ptr<fs::VFS>()> createVfs;
    std::function<std::unique_ptr<fs::Fs>(std::string_view, bool)> createPhysicalFs;
    std::function<std::unique_ptr<fs::Fs>(std::string_view)> createPackageFs;
    std::function<std::unique_ptr<audio::SoundManager>()> createSoundManager;
    std::function<std::shared_ptr<looper::Looper>()> createLooper;
    std::function<std::unique_ptr<FrameTimer>()> createFrameTimer;
};
} // namespace wallpaper
