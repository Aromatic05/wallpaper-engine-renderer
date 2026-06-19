#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "render/vulkan/include/vulkan/Device.hpp"
#include "render/vulkan/include/vulkan/VideoTextureCache.hpp"

#include <cassert>
#include <cmath>
#include <memory>

namespace
{
std::unique_ptr<wallpaper::Image> MakeVideoPlaceholderImage(std::string key) {
    auto image = std::make_unique<wallpaper::Image>();
    image->key = std::move(key);
    image->header.width = 1;
    image->header.height = 1;
    image->header.mapWidth = 1;
    image->header.mapHeight = 1;
    image->header.format = wallpaper::TextureFormat::RGBA8;
    image->header.sample = wallpaper::TextureSample {
        .wrapS = wallpaper::TextureWrap::CLAMP_TO_EDGE,
        .wrapT = wallpaper::TextureWrap::CLAMP_TO_EDGE,
        .magFilter = wallpaper::TextureFilter::LINEAR,
        .minFilter = wallpaper::TextureFilter::LINEAR,
    };
    image->header.isVideoTexture = true;
    image->slots.resize(1);
    image->slots[0].width = 1;
    image->slots[0].height = 1;

    auto pixels = std::make_unique<uint8_t[]>(4);
    pixels[0] = 0;
    pixels[1] = 0;
    pixels[2] = 0;
    pixels[3] = 255;
    wallpaper::ImageData mipmap;
    mipmap.width  = 1;
    mipmap.height = 1;
    mipmap.size   = 4;
    mipmap.data =
        wallpaper::ImageDataPtr(pixels.release(), [](uint8_t* ptr) { delete[] ptr; });
    image->slots[0].mipmaps.push_back(std::move(mipmap));
    return image;
}
} // namespace

int main() {
    wallpaper::Scene scene;
    scene.textures["movie"] = wallpaper::SceneTexture {
        .url = "movie",
        .sample =
            wallpaper::TextureSample {
                .wrapS = wallpaper::TextureWrap::CLAMP_TO_EDGE,
                .wrapT = wallpaper::TextureWrap::CLAMP_TO_EDGE,
                .magFilter = wallpaper::TextureFilter::LINEAR,
                .minFilter = wallpaper::TextureFilter::LINEAR,
            },
        .format = wallpaper::TextureFormat::RGBA8,
        .isVideo = true,
        .width = 1920,
        .height = 1080,
        .mapWidth = 1920,
        .mapHeight = 1080,
        .spriteAnim = {},
    };

    assert(scene.textures.at("movie").isVideo);
    assert(scene.textures.at("movie").width == 1920);

    wallpaper::vulkan::Device device;
    auto& cache = device.video_tex_cache();
    assert(cache.GetTrackedEntryCount() == 0);

    auto placeholder = MakeVideoPlaceholderImage("movie");
    auto slots = cache.Acquire("movie",
                               scene.textures.at("movie"),
                               *placeholder,
                               wallpaper::VideoTexturePlaybackState::Playing);
    assert(slots.slots.empty());
    assert(cache.GetTrackedEntryCount() == 1);
    assert(cache.GetTrackedBytes() == 4);

    scene.videoTexturePaused["movie"] = true;
    scene.videoTextureSeekRequests["movie"] = 3.5;

    cache.ApplyPlaybackStates(scene.videoTexturePaused, scene.videoTextureStopped);
    cache.ApplySeekRequests(scene.videoTextureSeekRequests);
    cache.Poll();

    assert(scene.videoTextureSeekRequests.empty());
    assert(cache.HasPipelineDiagnostic("movie"));
    assert(cache.GetAppliedSeekCount() == 1);
    assert(std::abs(cache.GetPlaybackSeconds("movie") - 3.5) < 0.0001);
    assert(cache.GetPendingUploadCount() == 1);

    vvk::CommandBuffer empty_command;
    cache.RecordUploads(empty_command);
    assert(cache.GetPendingUploadCount() == 0);
    assert(cache.GetRecordedUploadCount() == 1);

    scene.videoTexturePaused["movie"] = false;
    cache.ApplyPlaybackStates(scene.videoTexturePaused, scene.videoTextureStopped);
    cache.Poll();
    assert(cache.GetPlaybackSeconds("movie") > 3.5);
    assert(cache.GetPendingUploadCount() == 1);

    scene.videoTextureStopped.insert("movie");
    cache.ApplyPlaybackStates(scene.videoTexturePaused, scene.videoTextureStopped);
    const auto stopped_time = cache.GetPlaybackSeconds("movie");
    cache.Poll();
    assert(std::abs(cache.GetPlaybackSeconds("movie") - stopped_time) < 0.0001);

    assert(cache.Release("movie"));
    assert(cache.GetTrackedEntryCount() == 0);
    assert(cache.GetTrackedBytes() == 0);

    return 0;
}
