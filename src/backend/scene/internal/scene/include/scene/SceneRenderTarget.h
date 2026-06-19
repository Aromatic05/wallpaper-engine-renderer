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
    // Model reflection buffers are sampled by authored shaders that derive UVs from clip-space
    // screen coordinates rather than ordinary mesh texture coordinates. This opt-in sampling
    // convention lets material-bound slots fix that Y direction at shader preparation time without
    // changing how the producer pass renders or how legacy 2D effect targets are sampled.
    bool          screenSpaceSampleYFlip { false };
    bool          has_mipmap { false };
    uint          mipmap_level { 1 };
    TextureSample sample { TextureWrap::CLAMP_TO_EDGE,
                           TextureWrap::CLAMP_TO_EDGE,
                           TextureFilter::LINEAR,
                           TextureFilter::LINEAR };
    Bind          bind {};

    [[nodiscard]] i32 ContentWidth() const {
        // Render targets can expose a logical content rectangle that is smaller than their
        // allocated Vulkan image. This remains a generic render-target capability for authored
        // effect chains that intentionally decouple sampled content size from backing allocation.
        return mapWidth > 0 ? mapWidth : width;
    }

    [[nodiscard]] i32 ContentHeight() const {
        // Keep the fallback symmetric with ContentWidth() so targets that never set
        // mapWidth/mapHeight continue to behave exactly as before.
        return mapHeight > 0 ? mapHeight : height;
    }

    [[nodiscard]] std::array<i32, 4> ResolutionVector() const {
        return { width, height, ContentWidth(), ContentHeight() };
    }
};
} // namespace wallpaper
