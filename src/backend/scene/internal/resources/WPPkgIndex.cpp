#include "WPPkgIndex.hpp"

#include "fs/CBinaryStream.h"
#include "fs/IBinaryStream.h"

#include <cctype>
#include <limits>
#include <string>
#include <unordered_set>

namespace
{
constexpr wallpaper::i32 kMaxEntryCount = 1'000'000;
constexpr wallpaper::i32 kMaxStringSize = 16 * 1024 * 1024;

bool CanRead(const wallpaper::fs::IBinaryStream& stream, wallpaper::idx bytes) {
    if (bytes < 0) return false;
    const auto position = stream.Tell();
    const auto size     = stream.Size();
    return position >= 0 && size >= position && bytes <= size - position;
}

wallpaper::Result<wallpaper::i32> ReadInt32(wallpaper::fs::IBinaryStream& stream,
                                            std::string_view              label) {
    if (! CanRead(stream, static_cast<wallpaper::idx>(sizeof(wallpaper::i32)))) {
        return wallpaper::Result<wallpaper::i32>::failure(
            wallpaper::ResultCode::InvalidArgument,
            "truncated package header while reading " + std::string(label));
    }
    return wallpaper::Result<wallpaper::i32>::success(stream.ReadInt32());
}

wallpaper::Result<std::string> ReadSizedString(wallpaper::fs::IBinaryStream& stream,
                                               std::string_view              label) {
    auto lengthResult = ReadInt32(stream, std::string(label) + " length");
    if (! lengthResult) return wallpaper::Result<std::string>(lengthResult.error());

    const auto length = lengthResult.value();
    if (length < 0 || length > kMaxStringSize) {
        return wallpaper::Result<std::string>::failure(
            wallpaper::ResultCode::InvalidArgument,
            "invalid " + std::string(label) + " length");
    }
    if (! CanRead(stream, static_cast<wallpaper::idx>(length))) {
        return wallpaper::Result<std::string>::failure(
            wallpaper::ResultCode::InvalidArgument,
            "truncated package header while reading " + std::string(label));
    }

    std::string value(static_cast<std::size_t>(length), '\0');
    if (length > 0
        && stream.Read(value.data(), static_cast<wallpaper::usize>(length))
               != static_cast<wallpaper::usize>(length)) {
        return wallpaper::Result<std::string>::failure(
            wallpaper::ResultCode::InvalidArgument,
            "truncated package header while reading " + std::string(label));
    }
    return wallpaper::Result<std::string>::success(std::move(value));
}

bool IsPackageVersion(std::string_view version) {
    if (version.size() != 8 || ! version.starts_with("PKGV")) return false;
    for (const char ch : version.substr(4)) {
        if (! std::isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}
} // namespace

namespace wallpaper::fs
{
Result<WPPkgIndex> ReadWPPkgIndex(IBinaryStream& stream) {
    auto versionResult = ReadSizedString(stream, "package version");
    if (! versionResult) return Result<WPPkgIndex>(versionResult.error());
    if (! IsPackageVersion(versionResult.value())) {
        return Result<WPPkgIndex>::failure(ResultCode::InvalidArgument,
                                           "invalid package version stamp");
    }

    auto countResult = ReadInt32(stream, "entry count");
    if (! countResult) return Result<WPPkgIndex>(countResult.error());
    const auto entryCount = countResult.value();
    if (entryCount < 0 || entryCount > kMaxEntryCount) {
        return Result<WPPkgIndex>::failure(ResultCode::InvalidArgument,
                                           "invalid package entry count");
    }

    WPPkgIndex index;
    index.version = std::move(versionResult.value());
    index.entries.reserve(static_cast<std::size_t>(entryCount));
    std::unordered_set<std::string> paths;
    paths.reserve(static_cast<std::size_t>(entryCount));

    for (i32 entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
        auto pathResult = ReadSizedString(stream, "entry path");
        if (! pathResult) return Result<WPPkgIndex>(pathResult.error());
        if (pathResult.value().empty()) {
            return Result<WPPkgIndex>::failure(ResultCode::InvalidArgument,
                                               "package entry path is empty");
        }

        std::string path = std::move(pathResult.value());
        if (! path.starts_with('/')) path.insert(path.begin(), '/');
        if (! paths.insert(path).second) {
            return Result<WPPkgIndex>::failure(ResultCode::InvalidArgument,
                                               "duplicate package entry path: " + path);
        }

        auto offsetResult = ReadInt32(stream, "entry offset");
        if (! offsetResult) return Result<WPPkgIndex>(offsetResult.error());
        auto lengthResult = ReadInt32(stream, "entry length");
        if (! lengthResult) return Result<WPPkgIndex>(lengthResult.error());
        if (offsetResult.value() < 0 || lengthResult.value() < 0) {
            return Result<WPPkgIndex>::failure(ResultCode::InvalidArgument,
                                               "negative package entry range: " + path);
        }

        index.entries.push_back(WPPkgIndexEntry {
            std::move(path),
            static_cast<idx>(offsetResult.value()),
            static_cast<idx>(lengthResult.value()),
        });
    }

    index.headerSize = stream.Tell();
    const auto packageSize = stream.Size();
    if (index.headerSize < 0 || packageSize < index.headerSize) {
        return Result<WPPkgIndex>::failure(ResultCode::InvalidArgument,
                                           "invalid package header size");
    }

    for (auto& entry : index.entries) {
        if (entry.offset > packageSize - index.headerSize) {
            return Result<WPPkgIndex>::failure(ResultCode::InvalidArgument,
                                               "package entry offset is outside file: " + entry.path);
        }
        entry.offset += index.headerSize;
        if (entry.length > packageSize - entry.offset) {
            return Result<WPPkgIndex>::failure(ResultCode::InvalidArgument,
                                               "package entry length is outside file: " + entry.path);
        }
    }

    return Result<WPPkgIndex>::success(std::move(index));
}

Result<WPPkgIndex> ReadWPPkgIndex(std::string_view packagePath) {
    auto stream = CreateCBinaryStream(packagePath);
    if (! stream) {
        return Result<WPPkgIndex>::failure(ResultCode::NotFound,
                                           "cannot open package: " + std::string(packagePath));
    }
    return ReadWPPkgIndex(*stream);
}
} // namespace wallpaper::fs
