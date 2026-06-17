#pragma once

#include <string>
#include <vector>

namespace wallpaper
{
struct PropertySchemaEntry {
    std::string name;
    bool        runtimeMutable { true };
};

using PropertySchema = std::vector<PropertySchemaEntry>;
} // namespace wallpaper
