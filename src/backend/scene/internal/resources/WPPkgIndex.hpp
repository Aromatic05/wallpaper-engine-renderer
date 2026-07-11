#pragma once

#include "wallpaper/Result.hpp"
#include "core/Literals.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace wallpaper::fs
{
class IBinaryStream;

struct WPPkgIndexEntry {
    std::string path;
    idx         offset { 0 };
    idx         length { 0 };
};

struct WPPkgIndex {
    std::string                  version;
    idx                          headerSize { 0 };
    std::vector<WPPkgIndexEntry> entries;
};

Result<WPPkgIndex> ReadWPPkgIndex(IBinaryStream& stream);
Result<WPPkgIndex> ReadWPPkgIndex(std::string_view packagePath);
} // namespace wallpaper::fs
