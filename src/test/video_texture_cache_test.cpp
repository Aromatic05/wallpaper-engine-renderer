#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "render/vulkan/include/vulkan/Device.hpp"
#include "render/vulkan/include/vulkan/VideoTextureCache.hpp"

#include <cassert>

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

    scene.videoTexturePaused["movie"] = true;
    scene.videoTextureStopped.insert("movie");
    scene.videoTextureSeekRequests["movie"] = 3.5;

    cache.ApplyPlaybackStates(scene.videoTexturePaused, scene.videoTextureStopped);
    cache.SetGlobalPaused(true);
    cache.ApplySeekRequests(scene.videoTextureSeekRequests);
    cache.Poll();

    assert(scene.videoTextureSeekRequests.empty());
    assert(cache.GetTrackedEntryCount() == 0);
    assert(cache.GetTrackedBytes() == 0);

    return 0;
}
