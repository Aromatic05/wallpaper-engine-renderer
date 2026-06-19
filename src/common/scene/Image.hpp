#pragma once

#include "Type.hpp"
#include "scene/SpriteAnimation.hpp"
#include "core/Literals.hpp"
#include "core/NoCopyMove.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wallpaper
{
union ImageExtra {
    int32_t val { 0 };
    char    str[125];
};

using ImageDataPtr = std::unique_ptr<uint8_t, std::function<void(uint8_t*)>>;

struct ImageData {
    i32          width { 0 };
    i32          height { 0 };
    isize        size { 0 };
    ImageDataPtr data {};
};

struct ImageHeader {
    i32 width { 0 };
    i32 height { 0 };
    i32 mapWidth { 0 };
    i32 mapHeight { 0 };

    bool mipmap_larger { false };
    bool mipmap_pow2 { false };

    ImageType     type { ImageType::UNKNOWN };
    TextureFormat format { TextureFormat::RGBA8 };
    i32           count { 0 };

    bool          isSprite { false };
    bool          isVideoTexture { false };
    TextureSample sample;

    SpriteAnimation                           spriteAnim;
    std::unordered_map<std::string, ImageExtra> extraHeader;
};

struct Image : NoCopy, NoMove {
    struct Slot {
        i32 width { 0 };
        i32 height { 0 };

        std::vector<ImageData> mipmaps;

        operator bool() { return width * height * std::ssize(mipmaps) > 0; }
    };

    ImageHeader       header;
    std::vector<Slot> slots;
    std::string       key;
    uint64_t          revision { 0 };
};
} // namespace wallpaper
