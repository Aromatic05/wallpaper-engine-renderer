#include "backend/scene/internal/resources/WPPkgIndex.hpp"
#include "backend/scene/internal/resources/WPPkgFs.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace
{
using Bytes = std::vector<std::uint8_t>;

[[noreturn]] void Fail() { std::abort(); }
void Require(bool condition) {
    if (! condition) Fail();
}

void AppendInt32(Bytes& bytes, std::int32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(value) >> shift) & 0xffu));
    }
}

void AppendString(Bytes& bytes, std::string_view value) {
    AppendInt32(bytes, static_cast<std::int32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

Bytes BuildPackage(
    std::string_view version,
    const std::vector<std::pair<std::string, std::string>>& entries) {
    Bytes bytes;
    AppendString(bytes, version);
    AppendInt32(bytes, static_cast<std::int32_t>(entries.size()));

    std::int32_t bodyOffset = 0;
    for (const auto& [path, content] : entries) {
        AppendString(bytes, path);
        AppendInt32(bytes, bodyOffset);
        AppendInt32(bytes, static_cast<std::int32_t>(content.size()));
        bodyOffset += static_cast<std::int32_t>(content.size());
    }
    for (const auto& [path, content] : entries) {
        (void)path;
        bytes.insert(bytes.end(), content.begin(), content.end());
    }
    return bytes;
}

struct TempPackage {
    std::filesystem::path path;

    explicit TempPackage(const Bytes& bytes, std::string_view name = "scene.pkg") {
        const auto dir = std::filesystem::temp_directory_path()
            / ("we-pkg-index-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        path = dir / name;
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    ~TempPackage() {
        std::error_code error;
        std::filesystem::remove_all(path.parent_path(), error);
    }
};

void TestVersionMatrix() {
    for (int version = 1; version <= 23; ++version) {
        char stamp[9] {};
        std::snprintf(stamp, sizeof(stamp), "PKGV%04d", version);
        TempPackage package(BuildPackage(stamp, {}),
                            "version-" + std::to_string(version) + ".pkg");
        auto index = wallpaper::fs::ReadWPPkgIndex(package.path.string());
        Require(index.ok());
        Require(index.value().version == stamp);
        Require(index.value().entries.empty());
        Require(index.value().headerSize == 16);
    }
}

void TestEntriesAndAbsoluteRanges() {
    TempPackage package(BuildPackage(
        "PKGV0023",
        { { "scene.json", "scene" }, { "/materials/a.tex", "texture" } }));
    auto index = wallpaper::fs::ReadWPPkgIndex(package.path.string());
    Require(index.ok());
    Require(index.value().entries.size() == 2);
    Require(index.value().entries[0].path == "/scene.json");
    Require(index.value().entries[0].offset == index.value().headerSize);
    Require(index.value().entries[0].length == 5);
    Require(index.value().entries[1].path == "/materials/a.tex");
    Require(index.value().entries[1].offset == index.value().headerSize + 5);
    Require(index.value().entries[1].length == 7);

    auto packageFs = wallpaper::fs::WPPkgFs::CreatePkgFs(package.path.string());
    Require(packageFs != nullptr);
    Require(packageFs->Contains("/scene.json"));
    Require(packageFs->Contains("/materials/a.tex"));
    auto scene = packageFs->Open("/scene.json");
    auto texture = packageFs->Open("/materials/a.tex");
    Require(scene != nullptr && scene->ReadAllStr() == "scene");
    Require(texture != nullptr && texture->ReadAllStr() == "texture");
}

void TestMalformedHeaders() {
    {
        Bytes bytes;
        AppendInt32(bytes, 8);
        bytes.insert(bytes.end(), { 'P', 'K', 'G' });
        TempPackage package(bytes, "truncated-version.pkg");
        Require(! wallpaper::fs::ReadWPPkgIndex(package.path.string()));
    }
    {
        TempPackage package(BuildPackage("BADV0001", {}), "bad-version.pkg");
        Require(! wallpaper::fs::ReadWPPkgIndex(package.path.string()));
    }
    {
        Bytes bytes;
        AppendString(bytes, "PKGV0001");
        AppendInt32(bytes, -1);
        TempPackage package(bytes, "negative-count.pkg");
        Require(! wallpaper::fs::ReadWPPkgIndex(package.path.string()));
    }
    {
        TempPackage package(BuildPackage(
            "PKGV0001", { { "same", "a" }, { "same", "b" } }),
                            "duplicate.pkg");
        Require(! wallpaper::fs::ReadWPPkgIndex(package.path.string()));
    }
    {
        Bytes bytes;
        AppendString(bytes, "PKGV0001");
        AppendInt32(bytes, 1);
        AppendString(bytes, "scene.json");
        AppendInt32(bytes, 100);
        AppendInt32(bytes, 1);
        TempPackage package(bytes, "bad-offset.pkg");
        Require(! wallpaper::fs::ReadWPPkgIndex(package.path.string()));
        Require(wallpaper::fs::WPPkgFs::CreatePkgFs(package.path.string()) == nullptr);
    }
    {
        Bytes bytes;
        AppendString(bytes, "PKGV0001");
        AppendInt32(bytes, 1);
        AppendString(bytes, "scene.json");
        AppendInt32(bytes, 0);
        AppendInt32(bytes, -1);
        TempPackage package(bytes, "negative-length.pkg");
        Require(! wallpaper::fs::ReadWPPkgIndex(package.path.string()));
    }
}
} // namespace

int main() {
    const auto cacheRoot = std::filesystem::temp_directory_path()
        / ("we-pkg-index-cache-" + std::to_string(::getpid()));
    std::filesystem::create_directories(cacheRoot);
    ::setenv("XDG_CACHE_HOME", cacheRoot.c_str(), 1);

    TestVersionMatrix();
    TestEntriesAndAbsoluteRanges();
    TestMalformedHeaders();

    std::error_code error;
    std::filesystem::remove_all(cacheRoot, error);
    return 0;
}
