#pragma once

#include "wallpaper/Result.hpp"
#include "wallpaper/WallpaperTypes.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace wallpaper
{
struct ProjectSourceInfo {
    BackendType            type { BackendType::WEScene };
    std::filesystem::path projectJson;
    std::filesystem::path sourcePath;
    std::string           backendUri;
};

Result<ProjectSourceInfo> ResolveProjectSource(std::string_view uri);
} // namespace wallpaper
