#include "abi/WeRendererRuntime.hpp"

#include <array>
#include <cassert>
#include <cstddef>

int main() {
    we_runtime_settings_v1 settings {};
    settings.size = sizeof(settings);
    settings.version = 1;
    settings.fields = WE_RUNTIME_SETTINGS_FPS | WE_RUNTIME_SETTINGS_VOLUME
                      | WE_RUNTIME_SETTINGS_FILL_MODE;
    settings.fps = 60;
    settings.volume = 0.25f;
    settings.fill_mode = WE_FILL_MODE_CENTER;
    auto parsed = wallpaper::ParseRendererRuntimeSettings(&settings);
    assert(parsed);
    assert(parsed->fps == 60);
    assert(parsed->volume == 0.25f);
    assert(parsed->fillMode == WE_FILL_MODE_CENTER);

    settings.fps = 0;
    assert(! wallpaper::ParseRendererRuntimeSettings(&settings));
    settings.fps = 60;
    settings.fields = 1u << 31;
    assert(! wallpaper::ParseRendererRuntimeSettings(&settings));

    std::array<std::uint8_t, 8> rgba { 1, 2, 3, 4, 5, 6, 7, 8 };
    we_media_state_v1 media {};
    media.size = sizeof(media);
    media.version = 1;
    media.has_thumbnail = true;
    media.title = "title";
    media.thumbnail_width = 2;
    media.thumbnail_height = 1;
    media.thumbnail_rgba = rgba.data();
    auto parsedMedia = wallpaper::ParseRendererMediaState(&media);
    assert(parsedMedia);
    assert(parsedMedia->title == "title");
    assert(parsedMedia->thumbnailRgba.size() == rgba.size());

    media.thumbnail_rgba = nullptr;
    assert(! wallpaper::ParseRendererMediaState(&media));

    std::array<float, 4> audio { 0.0f, 0.25f, 0.5f, 1.0f };
    auto copied = wallpaper::CopyRendererAudioSamples(audio.data(), audio.size());
    assert(copied && copied->size() == audio.size());
    assert(! wallpaper::CopyRendererAudioSamples(nullptr, audio.size()));
    return 0;
}
