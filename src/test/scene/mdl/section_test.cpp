#include "backend/scene/internal/parser/mdl/Section.hpp"
#include "fs/MemBinaryStream.h"

#include <array>
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
    std::fprintf(stderr, "mdl section test failure: %.*s\n",
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

std::size_t BeginSection(Bytes& bytes, std::string_view type, int version) {
    Require(type.size() == 4, "section type must be four bytes");
    char stamp[9] {};
    std::snprintf(stamp, sizeof(stamp), "%.*s%04d",
                  static_cast<int>(type.size()), type.data(), version);
    bytes.insert(bytes.end(), stamp, stamp + sizeof(stamp));
    const std::size_t endOffsetPosition = bytes.size();
    AppendPod<std::uint32_t>(bytes, 0);
    return endOffsetPosition;
}

void EndSection(Bytes& bytes, std::size_t endOffsetPosition) {
    const auto end = static_cast<std::uint32_t>(bytes.size());
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&end);
    for (std::size_t index = 0; index < sizeof(end); ++index) {
        bytes[endOffsetPosition + index] = raw[index];
    }
}

void TestHeaderAndEndSeek() {
    Bytes bytes;
    const auto endPosition = BeginSection(bytes, "MDLS", 3);
    bytes.insert(bytes.end(), { 1, 2, 3, 4, 5 });
    EndSection(bytes, endPosition);

    wallpaper::fs::MemBinaryStream stream(std::move(bytes));
    auto header = wallpaper::ReadWPMdlSectionHeader(stream);
    Require(header.ok(), "valid section header should parse");
    Require(header.value().Is("MDLS"), "section type mismatch");
    Require(header.value().version == 3, "section version mismatch");
    Require(header.value().payload_offset == 13, "section payload offset mismatch");
    Require(header.value().end_offset == stream.Size(), "section end offset mismatch");

    std::uint8_t firstPayload = 0;
    Require(stream.Read(&firstPayload, 1) == 1 && firstPayload == 1,
            "section payload cursor mismatch");
    Require(wallpaper::SeekToWPMdlSectionEnd(stream, header.value()).ok(),
            "section end seek should succeed");
    Require(stream.Tell() == stream.Size(), "section end seek position mismatch");
}

void TestBoundedScanningAndUnknownSkip() {
    Bytes bytes { 0, 0, 0, 'M', 'D', 'L', 'S', 'x', 'x', 'x', 'x', 0 };
    const auto unknownEnd = BeginSection(bytes, "MDZZ", 7);
    AppendPod<std::uint32_t>(bytes, 0xdeadbeefu);
    EndSection(bytes, unknownEnd);
    const auto mdlaEnd = BeginSection(bytes, "MDLA", 2);
    AppendPod<std::uint32_t>(bytes, 0);
    EndSection(bytes, mdlaEnd);

    wallpaper::fs::MemBinaryStream stream(std::move(bytes));
    auto unknown = wallpaper::FindNextWPMdlSection(stream);
    Require(unknown.ok() && unknown.value().Is("MDZZ"),
            "scanner should find a structurally valid unknown section");
    Require(wallpaper::SeekToWPMdlSectionEnd(stream, unknown.value()).ok(),
            "unknown section should be skippable by end offset");

    constexpr std::array<std::string_view, 1> accepted { "MDLA" };
    auto mdla = wallpaper::FindNextWPMdlSection(stream, accepted);
    Require(mdla.ok() && mdla.value().version == 2,
            "scanner should find the requested section after an unknown section");
}

void TestMalformedHeaders() {
    {
        Bytes bytes(12, 0);
        wallpaper::fs::MemBinaryStream stream(std::move(bytes));
        auto result = wallpaper::ReadWPMdlSectionHeader(stream);
        Require(! result && result.error().message.find("truncated") != std::string::npos,
                "truncated section header must fail");
    }
    {
        Bytes bytes;
        const auto endPosition = BeginSection(bytes, "MDLS", 1);
        bytes.push_back(0);
        EndSection(bytes, endPosition);
        bytes[2] = '1';
        wallpaper::fs::MemBinaryStream stream(std::move(bytes));
        auto result = wallpaper::ReadWPMdlSectionHeader(stream);
        Require(! result && result.error().message.find("stamp") != std::string::npos,
                "invalid section type must fail");
    }
    {
        Bytes bytes;
        const auto endPosition = BeginSection(bytes, "MDLS", 1);
        bytes.push_back(0);
        EndSection(bytes, endPosition);
        const std::uint32_t outside = 1000;
        const auto* raw = reinterpret_cast<const std::uint8_t*>(&outside);
        for (std::size_t index = 0; index < sizeof(outside); ++index) {
            bytes[endPosition + index] = raw[index];
        }
        wallpaper::fs::MemBinaryStream stream(std::move(bytes));
        auto result = wallpaper::ReadWPMdlSectionHeader(stream);
        Require(! result && result.error().message.find("outside") != std::string::npos,
                "out-of-file section end must fail");
    }
    {
        Bytes bytes;
        const auto endPosition = BeginSection(bytes, "MDLA", 1);
        bytes.insert(bytes.end(), { 1, 2, 3 });
        EndSection(bytes, endPosition);
        wallpaper::fs::MemBinaryStream stream(std::move(bytes));
        auto header = wallpaper::ReadWPMdlSectionHeader(stream);
        Require(header.ok(), "test section should parse");
        Require(stream.SeekSet(header.value().end_offset), "test seek should succeed");
        std::uint8_t extra = 0;
        (void)stream.Read(&extra, 1);
        Require(wallpaper::SeekToWPMdlSectionEnd(stream, header.value()).ok(),
                "position at section end should be accepted");
    }
}
} // namespace

int main() {
    TestHeaderAndEndSeek();
    TestBoundedScanningAndUnknownSkip();
    TestMalformedHeaders();
    return 0;
}
