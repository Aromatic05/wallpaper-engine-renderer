#pragma once

#include <memory>
#include <string_view>

namespace wallpaper::fs
{
class Fs;
}

namespace wallpaper
{
std::unique_ptr<fs::Fs> CreateScenePackageFs(std::string_view pkgPath);
}
