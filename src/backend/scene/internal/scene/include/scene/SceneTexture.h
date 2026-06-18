#pragma once
#include "scene/SpriteAnimation.hpp"
#include <string>
#include <vector>
#include "Type.hpp"

namespace wallpaper
{

struct SceneTexture {
    std::string     url;
    TextureSample   sample;
    TextureFormat   format { TextureFormat::RGBA8 };
    bool            isVideo { false };
    bool            isSprite { false };
    i32             width { 0 };
    i32             height { 0 };
    i32             mapWidth { 0 };
    i32             mapHeight { 0 };
    SpriteAnimation spriteAnim;
};
} // namespace wallpaper
