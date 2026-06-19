#include "WPSceneScriptMedia.hpp"

#include <algorithm>
#include <atomic>

#include "Image.hpp"

namespace wallpaper
{
namespace
{

uint64_t NextSceneScriptMediaRevision() {
    static std::atomic<uint64_t> next_revision { 1 };
    return next_revision.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<Image> CreateImageWithStorage(std::string_view       key,
                                              int32_t                width,
                                              int32_t                height,
                                              std::unique_ptr<uint8_t[]> rgba) {
    if (width <= 0 || height <= 0 || rgba == nullptr) return nullptr;

    auto image = std::make_shared<Image>();
    image->key = std::string(key);
    image->revision = NextSceneScriptMediaRevision();
    image->header.width = width;
    image->header.height = height;
    image->header.mapWidth = width;
    image->header.mapHeight = height;
    image->header.count = 1;
    image->header.format = TextureFormat::RGBA8;
    image->header.type = ImageType::PNG;
    // Scene-script media images are generated at runtime instead of loaded from a .tex header.
    // Publish the same RGB component flags that ordinary RGBA textures expose so texture-driven
    // shader combos keep their sampler branches when bound to `$mediaThumbnail`.
    image->header.extraHeader["compo1"].val = 1;
    image->header.extraHeader["compo2"].val = 1;
    image->header.extraHeader["compo3"].val = 1;
    image->header.sample.wrapS = TextureWrap::CLAMP_TO_EDGE;
    image->header.sample.wrapT = TextureWrap::CLAMP_TO_EDGE;
    image->header.sample.magFilter = TextureFilter::LINEAR;
    image->header.sample.minFilter = TextureFilter::LINEAR;

    image->slots.resize(1);
    image->slots[0].width = width;
    image->slots[0].height = height;

    ImageData mipmap;
    mipmap.width = width;
    mipmap.height = height;
    mipmap.size = static_cast<isize>(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    mipmap.data = ImageDataPtr(rgba.release(), [](uint8_t* ptr) { delete[] ptr; });
    image->slots[0].mipmaps.push_back(std::move(mipmap));
    return image;
}

} // namespace

std::shared_ptr<Image> CreateSceneScriptSolidImage(std::string_view key,
                                                   const std::array<uint8_t, 4>& rgba) {
    auto pixels = std::make_unique<uint8_t[]>(4);
    std::copy(rgba.begin(), rgba.end(), pixels.get());
    return CreateImageWithStorage(key, 1, 1, std::move(pixels));
}

std::shared_ptr<Image> CreateSceneScriptRgbaImage(std::string_view       key,
                                                  int32_t                width,
                                                  int32_t                height,
                                                  std::span<const uint8_t> rgba) {
    const size_t expected_size =
        static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0)) * 4;
    if (expected_size == 0 || rgba.size() < expected_size) return nullptr;

    auto pixels = std::make_unique<uint8_t[]>(expected_size);
    std::copy_n(rgba.data(), expected_size, pixels.get());
    return CreateImageWithStorage(key, width, height, std::move(pixels));
}

} // namespace wallpaper
