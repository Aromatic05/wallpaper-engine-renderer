#include "Section.hpp"

#include "fs/IBinaryStream.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>

namespace wallpaper
{
namespace
{
Result<WPMdlSectionHeader> Invalid(std::string message) {
    return Result<WPMdlSectionHeader>::failure(ResultCode::InvalidArgument, std::move(message));
}

bool IsSectionTypeChar(char ch) {
    return ch >= 'A' && ch <= 'Z';
}

bool IsAccepted(std::string_view type, std::span<const std::string_view> acceptedTypes) {
    return acceptedTypes.empty()
           || std::find(acceptedTypes.begin(), acceptedTypes.end(), type) != acceptedTypes.end();
}
} // namespace

Result<WPMdlSectionHeader> ReadWPMdlSectionHeader(fs::IBinaryStream& stream) {
    const idx headerOffset = stream.Tell();
    if (headerOffset < 0 || stream.Size() - headerOffset < 13) {
        return Invalid("truncated MDL section header");
    }

    std::array<char, 9> stamp {};
    if (stream.Read(stamp.data(), stamp.size()) != stamp.size()) {
        return Invalid("truncated MDL section stamp");
    }
    if (stamp[0] != 'M' || stamp[1] != 'D'
        || ! IsSectionTypeChar(stamp[2]) || ! IsSectionTypeChar(stamp[3])
        || stamp[8] != '\0') {
        return Invalid("invalid MDL section stamp");
    }

    i32 version = 0;
    const auto [end, error] = std::from_chars(stamp.data() + 4, stamp.data() + 8, version);
    if (error != std::errc {} || end != stamp.data() + 8 || version <= 0) {
        return Invalid("invalid MDL section version");
    }

    const u32 rawEndOffset = stream.ReadUint32();
    const idx payloadOffset = stream.Tell();
    const idx endOffset = static_cast<idx>(rawEndOffset);
    if (endOffset < payloadOffset || endOffset > stream.Size()) {
        return Invalid("MDL section end offset is outside the file");
    }

    WPMdlSectionHeader header;
    std::copy_n(stamp.begin(), header.type.size(), header.type.begin());
    header.version = version;
    header.header_offset = headerOffset;
    header.payload_offset = payloadOffset;
    header.end_offset = endOffset;
    return Result<WPMdlSectionHeader>::success(header);
}

Result<WPMdlSectionHeader> FindNextWPMdlSection(
    fs::IBinaryStream& stream,
    std::span<const std::string_view> acceptedTypes,
    idx maxScanBytes) {
    const idx initial = stream.Tell();
    if (initial < 0 || maxScanBytes < 0) {
        return Invalid("invalid MDL section scan range");
    }

    const idx scanEnd = std::min(stream.Size(), initial + maxScanBytes);
    for (idx offset = initial; offset + 13 <= scanEnd; ++offset) {
        if (! stream.SeekSet(offset)) break;

        std::array<char, 4> prefix {};
        if (stream.Read(prefix.data(), prefix.size()) != prefix.size()) break;
        if (prefix[0] != 'M' || prefix[1] != 'D'
            || ! IsSectionTypeChar(prefix[2]) || ! IsSectionTypeChar(prefix[3])) {
            continue;
        }
        if (! stream.SeekSet(offset)) break;

        auto header = ReadWPMdlSectionHeader(stream);
        if (! header) continue;
        if (! IsAccepted(header.value().Type(), acceptedTypes)) continue;
        return header;
    }

    stream.SeekSet(initial);
    return Result<WPMdlSectionHeader>::failure(ResultCode::NotFound,
                                               "no valid MDL section found in scan range");
}

Result<void> SeekToWPMdlSectionEnd(fs::IBinaryStream& stream,
                                   const WPMdlSectionHeader& section) {
    const idx position = stream.Tell();
    if (position < section.payload_offset || position > section.end_offset) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "MDL section parser crossed its declared bounds");
    }
    if (position == section.end_offset) return Result<void>::success();
    if (! stream.SeekSet(section.end_offset)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "failed to seek to MDL section end");
    }
    return Result<void>::success();
}
} // namespace wallpaper
