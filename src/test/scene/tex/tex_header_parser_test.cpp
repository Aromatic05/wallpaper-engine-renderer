#include "backend/scene/internal/parser/WPTexHeaderParser.hpp"
#include "fs/MemBinaryStream.h"

#include <bit>
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
    std::fprintf(stderr, "tex header test failure: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

template<typename T>
void AppendPod(Bytes& bytes, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(value));
}

void AppendStamp(Bytes& bytes, char section, int version) {
    char stamp[9] {};
    std::snprintf(stamp, sizeof(stamp), "TEX%c%04d", section, version);
    bytes.insert(bytes.end(), stamp, stamp + sizeof(stamp));
}

Bytes BuildTexture(int texb, bool sprite = false) {
    Bytes bytes;
    AppendStamp(bytes, 'V', 5);
    AppendStamp(bytes, 'I', 1);
    AppendPod<std::int32_t>(bytes, 0);
    AppendPod<std::uint32_t>(bytes, sprite ? (1u << 2) : 0u);
    AppendPod<std::int32_t>(bytes, 16);
    AppendPod<std::int32_t>(bytes, 8);
    AppendPod<std::int32_t>(bytes, 16);
    AppendPod<std::int32_t>(bytes, 8);
    AppendPod<std::int32_t>(bytes, 0);
    AppendStamp(bytes, 'B', texb);
    AppendPod<std::int32_t>(bytes, 1);
    if (texb >= 3) AppendPod<std::int32_t>(bytes, -1);
    if (texb >= 4) AppendPod<std::int32_t>(bytes, 1234);

    AppendPod<std::int32_t>(bytes, 1);
    AppendPod<std::int32_t>(bytes, 16);
    AppendPod<std::int32_t>(bytes, 8);
    if (texb >= 2) {
        AppendPod<std::int32_t>(bytes, 0);
        AppendPod<std::int32_t>(bytes, 16 * 8 * 4);
    }
    AppendPod<std::int32_t>(bytes, 16);
    const std::uint8_t payload[16] {
        0, 0, 0, 16, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 0, 0
    };
    bytes.insert(bytes.end(), std::begin(payload), std::end(payload));

    if (sprite) {
        AppendStamp(bytes, 'S', 3);
        AppendPod<std::int32_t>(bytes, 1);
        AppendPod<std::int32_t>(bytes, 16);
        AppendPod<std::int32_t>(bytes, 8);
        AppendPod<std::int32_t>(bytes, 0);
        AppendPod<float>(bytes, 0.1f);
        AppendPod<float>(bytes, 0.0f);
        AppendPod<float>(bytes, 0.0f);
        AppendPod<float>(bytes, 16.0f);
        AppendPod<float>(bytes, 0.0f);
        AppendPod<float>(bytes, 0.0f);
        AppendPod<float>(bytes, 8.0f);
    }
    return bytes;
}

wallpaper::Result<wallpaper::ImageHeader> Parse(Bytes bytes) {
    wallpaper::fs::MemBinaryStream stream(std::move(bytes));
    return wallpaper::ParseWPTexHeader(stream);
}

void TestTexbVersions() {
    for (int texb = 1; texb <= 4; ++texb) {
        auto result = Parse(BuildTexture(texb));
        Require(result.ok(), "TEXB1-4 should parse");
        const auto& header = result.value();
        Require(header.extraHeader.at("texv").val == 5, "TEXV mismatch");
        Require(header.extraHeader.at("texi").val == 1, "TEXI mismatch");
        Require(header.extraHeader.at("texb").val == texb, "TEXB mismatch");
        Require(header.width == 16 && header.height == 8, "header dimensions mismatch");
        Require(header.mapWidth == 16 && header.mapHeight == 8, "map dimensions mismatch");
        Require(header.count == 1, "image count mismatch");
        Require(header.isVideoTexture, "MP4 payload should be detected");
        if (texb == 4) {
            Require(header.extraHeader.at("texb_reserved").val == 1234,
                    "TEXB4 reserved field mismatch");
        }
    }
}

void TestSpriteMetadata() {
    auto result = Parse(BuildTexture(4, true));
    Require(result.ok(), "TEXS3 sprite should parse");
    const auto& header = result.value();
    Require(header.isSprite, "sprite flag missing");
    Require(header.extraHeader.at("texs").val == 3, "TEXS version mismatch");
    Require(header.spriteAnim.Frames().size() == 1, "sprite frame count mismatch");
    const auto& frame = header.spriteAnim.Frames().front();
    Require(frame.imageId == 0, "sprite image id mismatch");
    Require(frame.width == 16.0f && frame.height == 8.0f,
            "sprite frame axes mismatch");
}

void TestFailures() {
    {
        auto bytes = BuildTexture(1);
        bytes.pop_back();
        auto result = Parse(std::move(bytes));
        Require(! result && result.error().code == wallpaper::ResultCode::InvalidArgument,
                "truncated payload must fail");
    }
    {
        auto result = Parse(BuildTexture(5));
        Require(! result && result.error().code == wallpaper::ResultCode::NotSupported,
                "TEXB5 must be reported as unsupported");
    }
    {
        auto bytes = BuildTexture(2);
        const std::size_t sourceSizeOffset =
            9 + 9 + 4 + 4 + 20 + 9 + 4 + 4 + 4 + 4 + 4 + 4;
        const std::int32_t oversized = 1'000'000;
        const auto* raw = reinterpret_cast<const std::uint8_t*>(&oversized);
        for (std::size_t i = 0; i < sizeof(oversized); ++i) {
            bytes[sourceSizeOffset + i] = raw[i];
        }
        auto result = Parse(std::move(bytes));
        Require(! result && result.error().message.find("exceeds stream bounds") != std::string::npos,
                "oversized payload must fail with bounds error");
    }
    {
        auto bytes = BuildTexture(4, true);
        const std::size_t imageIdOffset = bytes.size() - (sizeof(float) * 7 + sizeof(std::int32_t));
        const std::int32_t badImageId = 7;
        const auto* raw = reinterpret_cast<const std::uint8_t*>(&badImageId);
        for (std::size_t i = 0; i < sizeof(badImageId); ++i) {
            bytes[imageIdOffset + i] = raw[i];
        }
        auto result = Parse(std::move(bytes));
        Require(! result && result.error().message.find("image id") != std::string::npos,
                "invalid sprite image id must fail");
    }
}
} // namespace

int main() {
    TestTexbVersions();
    TestSpriteMetadata();
    TestFailures();
    return 0;
}
