#pragma once

#include <cassert>
#include <cstdint>
#include <string_view>
#include <vector>

namespace wallpaper::test
{
inline constexpr std::string_view kFixtureVideoBase64 =
    "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";

inline int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
}

inline std::vector<std::uint8_t> DecodeBase64(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    int quartet[4] = { 0, 0, 0, 0 };
    int quartet_size = 0;

    for (char c : text) {
        const int value = Base64Value(c);
        if (value == -1) continue;
        quartet[quartet_size++] = value;
        if (quartet_size != 4) continue;

        assert(quartet[0] >= 0 && quartet[1] >= 0);
        bytes.push_back(static_cast<std::uint8_t>((quartet[0] << 2) | (quartet[1] >> 4)));
        if (quartet[2] != -2) {
            bytes.push_back(
                static_cast<std::uint8_t>(((quartet[1] & 0x0f) << 4) | (quartet[2] >> 2)));
        }
        if (quartet[3] != -2) {
            bytes.push_back(
                static_cast<std::uint8_t>(((quartet[2] & 0x03) << 6) | quartet[3]));
        }
        quartet_size = 0;
    }

    return bytes;
}
} // namespace wallpaper::test
