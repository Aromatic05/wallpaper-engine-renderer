#pragma once

#include "api/scene/WEScene.hpp"
#include "runtime/session/WallpaperTypes.hpp"

namespace wallpaper
{
WallpaperSource MakeWESceneWallpaperSource(const WESceneSourceConfig& config);
} // namespace wallpaper
