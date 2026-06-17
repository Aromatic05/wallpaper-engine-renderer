#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>

namespace wallpaper
{
using PropertyObject = std::shared_ptr<void>;
using PropertyValue =
    std::variant<std::monostate, bool, std::int32_t, float, double, std::string, PropertyObject>;
using PropertyMap   = std::unordered_map<std::string, PropertyValue>;
} // namespace wallpaper
