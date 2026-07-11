#include "backend/scene/internal/parser/mdl/Format.hpp"
#include "fs/MemBinaryStream.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
using Bytes = std::vector<std::uint8_t>;

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr, "mdl format test failure: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

template<typename T>
void AppendPod(Bytes& bytes, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

Bytes BuildHeader(int version,
                  std::uint32_t flags = wallpaper::WPMDL_FLAG_POSITION,
                  std::uint32_t unknown = 1,
                  std::uint32_t meshCount = 1) {
    Bytes bytes;
    char stamp[9] {};
    std::snprintf(stamp, sizeof(stamp), "MDLV%04d", version);
    bytes.insert(bytes.end(), stamp, stamp + sizeof(stamp));
    AppendPod(bytes, flags);
    AppendPod(bytes, unknown);
    AppendPod(bytes, meshCount);
    return bytes;
}

wallpaper::Result<wallpaper::WPMdlHeader> Parse(Bytes bytes) {
    wallpaper::fs::MemBinaryStream stream(std::move(bytes));
    return wallpaper::ParseWPMdlHeader(stream);
}

void TestObservedVersions() {
    for (const int version : { 4, 13, 14, 16, 17, 21, 23 }) {
        auto result = Parse(BuildHeader(version, 0x01800009u, 1, 2));
        Require(result.ok(), "observed MDLV header should parse");
        Require(result.value().mdlv == version, "MDLV version mismatch");
        Require(result.value().mdl_flag == 0x01800009u, "MDL flags mismatch");
        Require(result.value().unk_a == 1, "MDL unknown header field mismatch");
        Require(result.value().mesh_count == 2, "MDL mesh count mismatch");
    }
}

void TestVertexStrides() {
    Require(wallpaper::WPMdlVertexStride(wallpaper::WPMDL_FLAG_POSITION) == 12,
            "position-only stride mismatch");
    Require(wallpaper::WPMdlVertexStride(0x01800009u) == 52,
            "puppet position/skin/uv stride mismatch");
    Require(wallpaper::WPMdlVertexStride(0x0180000fu) == 80,
            "extended puppet stride mismatch");
    Require(wallpaper::WPMdlVertexStride(0x0000000fu) == 48,
            "static image stride mismatch");
    Require(wallpaper::WPMdlVertexStride(
                wallpaper::WPMDL_FLAG_POSITION | wallpaper::WPMDL_FLAG_UV2)
                == 28,
            "UV2 must include primary and secondary UV slots");
}

void TestMalformedHeaders() {
    {
        auto bytes = BuildHeader(13);
        bytes.resize(12);
        auto result = Parse(std::move(bytes));
        Require(! result && result.error().code == wallpaper::ResultCode::InvalidArgument,
                "truncated header must fail");
    }
    {
        auto bytes = BuildHeader(13);
        bytes[0] = 'X';
        auto result = Parse(std::move(bytes));
        Require(! result && result.error().message.find("stamp") != std::string::npos,
                "invalid stamp must fail");
    }
    {
        auto result = Parse(BuildHeader(24));
        Require(! result && result.error().code == wallpaper::ResultCode::NotSupported,
                "future MDLV must be reported as unsupported");
    }
    {
        auto result = Parse(BuildHeader(13, wallpaper::WPMDL_FLAG_POSITION, 1, 0));
        Require(! result && result.error().message.find("mesh count") != std::string::npos,
                "zero mesh count must fail");
    }
    {
        auto result = Parse(BuildHeader(13, wallpaper::WPMDL_FLAG_POSITION, 1, 65'537));
        Require(! result && result.error().message.find("mesh count") != std::string::npos,
                "implausible mesh count must fail");
    }
}
} // namespace

int main() {
    TestObservedVersions();
    TestVertexStrides();
    TestMalformedHeaders();
    return 0;
}
