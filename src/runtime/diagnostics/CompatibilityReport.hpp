#pragma once

#include <string>
#include <vector>

namespace wallpaper
{
struct CompatibilityReport {
    bool                     compatible { true };
    std::vector<std::string> notes;
};
} // namespace wallpaper
