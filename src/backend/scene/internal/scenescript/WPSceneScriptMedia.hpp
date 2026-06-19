#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wallpaper
{

struct Image;

struct WPSceneScriptMediaState {
    bool                 has_thumbnail { false };
    int32_t              playback_state { 0 };
    std::array<float, 3> primary_color { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> secondary_color { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> tertiary_color { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> text_color { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> high_contrast_color { 1.0f, 1.0f, 1.0f };
    std::string          title;
    std::string          artist;
    std::string          album_title;
    std::string          album_artist;
    std::string          sub_title;
    std::string          genres;
    std::string          content_type;
    int32_t              thumbnail_width { 0 };
    int32_t              thumbnail_height { 0 };
    std::vector<uint8_t> thumbnail_rgba;
    int32_t              previous_thumbnail_width { 0 };
    int32_t              previous_thumbnail_height { 0 };
    std::vector<uint8_t> previous_thumbnail_rgba;
};

constexpr std::string_view WP_SCENE_SCRIPT_MEDIA_THUMBNAIL_TEXTURE { "__scenescript_media_thumbnail" };
constexpr std::string_view WP_SCENE_SCRIPT_MEDIA_PREVIOUS_THUMBNAIL_TEXTURE {
    "__scenescript_media_previous_thumbnail"
};

std::shared_ptr<Image> CreateSceneScriptSolidImage(std::string_view key,
                                                   const std::array<uint8_t, 4>& rgba);
std::shared_ptr<Image> CreateSceneScriptRgbaImage(std::string_view      key,
                                                  int32_t               width,
                                                  int32_t               height,
                                                  std::span<const uint8_t> rgba);

} // namespace wallpaper
