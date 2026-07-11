#pragma once

#include "wallpaper/Result.hpp"
#include "scene/Image.hpp"

namespace wallpaper::fs
{
class IBinaryStream;
}

namespace wallpaper
{
Result<ImageHeader> ParseWPTexHeader(fs::IBinaryStream& stream);
} // namespace wallpaper
