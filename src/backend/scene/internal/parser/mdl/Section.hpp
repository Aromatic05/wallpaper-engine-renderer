#pragma once

#include "wallpaper/Result.hpp"
#include "core/Literals.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>

namespace wallpaper::fs
{
class IBinaryStream;
}

namespace wallpaper
{
struct WPMdlSectionHeader {
    std::array<char, 4> type {};
    i32 version { 0 };
    idx header_offset { 0 };
    idx payload_offset { 0 };
    idx end_offset { 0 };

    std::string Type() const { return std::string(type.data(), type.size()); }
    bool Is(std::string_view expected) const {
        return expected.size() == type.size()
               && std::equal(type.begin(), type.end(), expected.begin());
    }
};

Result<WPMdlSectionHeader> ReadWPMdlSectionHeader(fs::IBinaryStream& stream);
Result<WPMdlSectionHeader> FindNextWPMdlSection(
    fs::IBinaryStream& stream,
    std::span<const std::string_view> acceptedTypes = {},
    idx maxScanBytes = 1024 * 1024);
Result<void> SeekToWPMdlSectionEnd(fs::IBinaryStream& stream,
                                   const WPMdlSectionHeader& section);
} // namespace wallpaper
