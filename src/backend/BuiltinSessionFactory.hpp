#pragma once

#include "runtime/WallpaperRuntime.hpp"

#include <memory>
#include <string>

namespace wallpaper
{
SessionConfig MakeBuiltinSessionConfig(std::string cachePath = {});

std::unique_ptr<WallpaperSession> CreateBuiltinSession(WallpaperRuntime& runtime,
                                                       std::string       cachePath = {});
} // namespace wallpaper
