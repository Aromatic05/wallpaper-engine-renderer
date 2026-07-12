#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wallpaper
{
struct MediaState {
    bool                 hasThumbnail { false };
    std::int32_t         playbackState { 0 };
    std::array<float, 3> primaryColor { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> secondaryColor { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> tertiaryColor { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> textColor { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> highContrastColor { 1.0f, 1.0f, 1.0f };
    std::string          title;
    std::string          artist;
    std::string          albumTitle;
    std::string          albumArtist;
    std::string          subTitle;
    std::string          genres;
    std::string          contentType;
    std::uint32_t        thumbnailWidth { 0 };
    std::uint32_t        thumbnailHeight { 0 };
    std::vector<std::uint8_t> thumbnailRgba;
    std::uint32_t        previousThumbnailWidth { 0 };
    std::uint32_t        previousThumbnailHeight { 0 };
    std::vector<std::uint8_t> previousThumbnailRgba;
};
} // namespace wallpaper
