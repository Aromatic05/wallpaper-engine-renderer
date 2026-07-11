#pragma once

#include "WPUserProperties.hpp"
#include "wallpaper/Result.hpp"

#include <string_view>

namespace wallpaper
{
Result<UserPropertyMap> ParseUserPropertiesJson(std::string_view jsonText);
UserPropertyMap MergeUserPropertiesWithDefaults(const UserPropertyMap& defaults,
                                                const UserPropertyMap& overrides);
} // namespace wallpaper
