#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>

namespace wallpaper
{
using PropertyValue = std::variant<std::monostate, bool, std::int32_t, float, double, std::string>;
using PropertyMap   = std::unordered_map<std::string, PropertyValue>;
} // namespace wallpaper
