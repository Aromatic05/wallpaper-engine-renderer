#pragma once

#include "wallpaper/Result.hpp"
#include "wallpaper/WallpaperTypes.hpp"

#include <string>
#include <string_view>

namespace wallpaper
{
Result<std::string> NormalizeUserPropertiesJson(std::string_view jsonText);
Result<void> ApplyRendererSourceOptionsJson(std::string_view jsonText, WallpaperSource& source);
} // namespace wallpaper
