#pragma once

#include "utils/Platform.hpp"

#include <string>
#include <string_view>

namespace wallpaper
{
namespace host
{
inline std::string GetCachePath(std::string_view name) {
    return platform::GetCachePath(name).string();
}
} // namespace host
} // namespace wallpaper
