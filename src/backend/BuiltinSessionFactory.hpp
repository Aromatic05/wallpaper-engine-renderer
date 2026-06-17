#pragma once

#include "api/WallpaperRuntime.hpp"

#include <memory>
#include <string>

namespace wallpaper
{
std::unique_ptr<WallpaperSession> CreateBuiltinSession(WallpaperRuntime& runtime,
                                                       std::string       cachePath = {});
} // namespace wallpaper
