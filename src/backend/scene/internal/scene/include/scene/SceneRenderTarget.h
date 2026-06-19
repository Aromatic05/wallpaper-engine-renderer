#pragma once
#include "SceneTexture.h"
#include "core/Literals.hpp"
#include <array>

namespace wallpaper
{

struct SceneRenderTarget {
    struct Bind {
        bool        enable { false };
        std::string name {};
        bool        screen { false };
        double      scale { 1.0 };
    };

    i32           width;
    i32           height;
    i32           mapWidth { 0 };
    i32           mapHeight { 0 };
    bool          allowReuse { false };
    bool          withDepth { false };
    bool          has_mipmap { false };
    uint          mipmap_level { 1 };
    TextureSample sample { TextureWrap::CLAMP_TO_EDGE,
                           TextureWrap::CLAMP_TO_EDGE,
                           TextureFilter::LINEAR,
                           TextureFilter::LINEAR };
    Bind          bind {};

    [[nodiscard]] i32 ContentWidth() const { return mapWidth > 0 ? mapWidth : width; }
    [[nodiscard]] i32 ContentHeight() const { return mapHeight > 0 ? mapHeight : height; }
    [[nodiscard]] std::array<i32, 4> ResolutionVector() const {
        return { width, height, ContentWidth(), ContentHeight() };
    }
};
} // namespace wallpaper
