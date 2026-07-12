#pragma once

#include "wallpaper/MediaState.hpp"
#include "wallpaper/abi/WeRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wallpaper
{
struct RendererRuntimeSettings {
    std::uint32_t fields { 0 };
    std::int32_t fps { 0 };
    float speed { 1.0f };
    float volume { 1.0f };
    bool muted { false };
    we_fill_mode_v1 fillMode { WE_FILL_MODE_ASPECT_CROP };
};

inline bool RuntimeSettingsHasField(const we_runtime_settings_v1* settings,
                                    std::size_t fieldOffset,
                                    std::size_t fieldSize) noexcept {
    return settings != nullptr && settings->size >= fieldOffset + fieldSize;
}

inline bool ValidFillMode(we_fill_mode_v1 fillMode) noexcept {
    return fillMode == WE_FILL_MODE_ASPECT_CROP || fillMode == WE_FILL_MODE_STRETCH
           || fillMode == WE_FILL_MODE_ASPECT_FIT || fillMode == WE_FILL_MODE_CENTER;
}

inline std::optional<RendererRuntimeSettings> ParseRendererRuntimeSettings(
    const we_runtime_settings_v1* settings) noexcept {
    if (settings == nullptr || settings->version != 1
        || ! RuntimeSettingsHasField(settings,
                                    offsetof(we_runtime_settings_v1, fields),
                                    sizeof(settings->fields))) {
        return std::nullopt;
    }
    constexpr std::uint32_t kKnownFields = WE_RUNTIME_SETTINGS_FPS | WE_RUNTIME_SETTINGS_SPEED
                                           | WE_RUNTIME_SETTINGS_VOLUME | WE_RUNTIME_SETTINGS_MUTED
                                           | WE_RUNTIME_SETTINGS_FILL_MODE;
    if ((settings->fields & ~kKnownFields) != 0 || settings->fields == 0) return std::nullopt;

    RendererRuntimeSettings result;
    result.fields = settings->fields;
    if ((settings->fields & WE_RUNTIME_SETTINGS_FPS) != 0) {
        if (! RuntimeSettingsHasField(settings,
                                     offsetof(we_runtime_settings_v1, fps),
                                     sizeof(settings->fps))
            || settings->fps < 5 || settings->fps > 240) {
            return std::nullopt;
        }
        result.fps = settings->fps;
    }
    if ((settings->fields & WE_RUNTIME_SETTINGS_SPEED) != 0) {
        if (! RuntimeSettingsHasField(settings,
                                     offsetof(we_runtime_settings_v1, speed),
                                     sizeof(settings->speed))
            || ! std::isfinite(settings->speed) || settings->speed <= 0.0f) {
            return std::nullopt;
        }
        result.speed = settings->speed;
    }
    if ((settings->fields & WE_RUNTIME_SETTINGS_VOLUME) != 0) {
        if (! RuntimeSettingsHasField(settings,
                                     offsetof(we_runtime_settings_v1, volume),
                                     sizeof(settings->volume))
            || ! std::isfinite(settings->volume) || settings->volume < 0.0f
            || settings->volume > 1.0f) {
            return std::nullopt;
        }
        result.volume = settings->volume;
    }
    if ((settings->fields & WE_RUNTIME_SETTINGS_MUTED) != 0) {
        if (! RuntimeSettingsHasField(settings,
                                     offsetof(we_runtime_settings_v1, muted),
                                     sizeof(settings->muted))) {
            return std::nullopt;
        }
        result.muted = settings->muted;
    }
    if ((settings->fields & WE_RUNTIME_SETTINGS_FILL_MODE) != 0) {
        if (! RuntimeSettingsHasField(settings,
                                     offsetof(we_runtime_settings_v1, fill_mode),
                                     sizeof(settings->fill_mode))
            || ! ValidFillMode(settings->fill_mode)) {
            return std::nullopt;
        }
        result.fillMode = settings->fill_mode;
    }
    return result;
}

inline std::optional<std::size_t> RgbaByteCount(std::uint32_t width,
                                                std::uint32_t height) noexcept {
    if (width == 0 || height == 0) return std::size_t { 0 };
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 4u) return std::nullopt;
    return static_cast<std::size_t>(pixels * 4u);
}

inline std::string StringOrEmpty(const char* value) { return value ? std::string(value) : std::string(); }

inline std::optional<MediaState> ParseRendererMediaState(const we_media_state_v1* state) {
    if (state == nullptr || state->version != 1 || state->size < sizeof(we_media_state_v1)) {
        return std::nullopt;
    }
    const auto thumbnailBytes = RgbaByteCount(state->thumbnail_width, state->thumbnail_height);
    const auto previousBytes =
        RgbaByteCount(state->previous_thumbnail_width, state->previous_thumbnail_height);
    if (! thumbnailBytes || ! previousBytes) return std::nullopt;
    if ((*thumbnailBytes != 0 && state->thumbnail_rgba == nullptr)
        || (*previousBytes != 0 && state->previous_thumbnail_rgba == nullptr)) {
        return std::nullopt;
    }

    MediaState result;
    result.hasThumbnail = state->has_thumbnail;
    result.playbackState = state->playback_state;
    std::copy_n(state->primary_color, 3, result.primaryColor.begin());
    std::copy_n(state->secondary_color, 3, result.secondaryColor.begin());
    std::copy_n(state->tertiary_color, 3, result.tertiaryColor.begin());
    std::copy_n(state->text_color, 3, result.textColor.begin());
    std::copy_n(state->high_contrast_color, 3, result.highContrastColor.begin());
    result.title = StringOrEmpty(state->title);
    result.artist = StringOrEmpty(state->artist);
    result.albumTitle = StringOrEmpty(state->album_title);
    result.albumArtist = StringOrEmpty(state->album_artist);
    result.subTitle = StringOrEmpty(state->sub_title);
    result.genres = StringOrEmpty(state->genres);
    result.contentType = StringOrEmpty(state->content_type);
    result.thumbnailWidth = state->thumbnail_width;
    result.thumbnailHeight = state->thumbnail_height;
    if (*thumbnailBytes != 0) {
        result.thumbnailRgba.assign(state->thumbnail_rgba,
                                    state->thumbnail_rgba + *thumbnailBytes);
    }
    result.previousThumbnailWidth = state->previous_thumbnail_width;
    result.previousThumbnailHeight = state->previous_thumbnail_height;
    if (*previousBytes != 0) {
        result.previousThumbnailRgba.assign(state->previous_thumbnail_rgba,
                                            state->previous_thumbnail_rgba + *previousBytes);
    }
    return result;
}

inline std::optional<std::vector<float>> CopyRendererAudioSamples(const float* samples,
                                                                  std::uint32_t count) {
    if (samples == nullptr || count == 0 || count > 4096) return std::nullopt;
    std::vector<float> result(samples, samples + count);
    if (! std::all_of(result.begin(), result.end(), [](float value) {
            return std::isfinite(value);
        })) {
        return std::nullopt;
    }
    return result;
}
} // namespace wallpaper
