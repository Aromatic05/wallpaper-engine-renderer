#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Image.hpp"
#include "SceneMesh.h"
#include "wpscene/WPTextObject.h"

namespace wallpaper
{

struct TextGlyphRun {
    uint32_t             page_index { 0 };
    std::array<float, 4> source_rect { 0.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, 4> atlas_rect { 0.0f, 0.0f, 0.0f, 0.0f };
};

struct TextGlyphAtlasPage {
    std::string            texture_key;
    std::shared_ptr<Image> image;
    std::array<float, 2>   source_size { 0.0f, 0.0f };
};

struct TextLayoutResult {
    // The canonical logical box always tracks the authored text rectangle after shaping. This is
    // the box used by opaque-background rendering and by exact-size text bridges.
    std::array<float, 2> logical_size { 0.0f, 0.0f };
    std::array<float, 2> logical_source_size { 0.0f, 0.0f };

    // Glyph-only text can expose cropped glyph bounds as visible geometry, but placement still
    // belongs to `logical_size`. The cropped visible quad starts from its measured local offset from
    // that logical rectangle; WPTextLayer resolves the final mesh center from these crop metrics and
    // the authored alignment/origin.
    std::array<float, 2> glyph_display_size { 0.0f, 0.0f };
    std::array<float, 2> glyph_source_size { 0.0f, 0.0f };
    std::array<float, 2> glyph_offset { 0.0f, 0.0f };
    std::array<float, 2> visible_display_size { 0.0f, 0.0f };
    std::array<float, 2> visible_source_size { 0.0f, 0.0f };
    std::array<float, 2> visible_display_offset { 0.0f, 0.0f };

    std::vector<TextGlyphAtlasPage> glyph_pages;
    std::vector<TextGlyphRun>       glyph_runs;
};

struct TextBridgeRenderTarget {
    // A first-class text bridge owns exact-size offscreen targets that image effects sample from.
    // `scale` and `fit` mirror Wallpaper Engine's authored effect FBO sizing rules so runtime text
    // updates can recompute the same target dimensions that parse-time image/text effect material
    // construction used. Keeping the sizing metadata with the bridge avoids texture-resolution
    // hacks inside shader passes when an effect target is resized after text content changes.
    std::string name;
    uint32_t    scale { 1 };
    uint32_t    fit { 0 };
};

struct TextSourceBridge {
    bool                 enabled { false };
    std::string          camera_name;
    std::string          pingpong_a;
    std::string          pingpong_b;
    std::array<float, 2> source_size { 0.0f, 0.0f };
    std::vector<TextBridgeRenderTarget> render_targets;
};

class SceneTextPrimitive {
public:
    struct GlyphPageRenderable {
        uint32_t                 page_index { 0 };
        std::string              texture_key;
        std::array<float, 2>     source_size { 0.0f, 0.0f };
        std::shared_ptr<SceneMesh> mesh;
    };

    // The primitive is scene-owned state: authored text properties, canonical layout, atlas page
    // meshes, material data, and optional bridge metadata all live here so parser, runtime, and
    // render graph consume one final representation instead of synthetic image-layer sidecars.
    wpscene::WPTextObject object;
    TextLayoutResult      layout;
    TextSourceBridge      bridge;
    std::shared_ptr<SceneMesh> background_mesh;
    std::vector<GlyphPageRenderable> glyph_pages;
    uint32_t              atlas_version { 0 };

    [[nodiscard]] std::array<float, 2> VisibleDisplaySize() const { return layout.visible_display_size; }
    [[nodiscard]] std::array<float, 2> VisibleSourceSize() const { return layout.visible_source_size; }
    [[nodiscard]] std::array<float, 2> VisibleDisplayOffset() const { return layout.visible_display_offset; }
    [[nodiscard]] std::array<float, 2> BackgroundLocalOffset() const {
        return object.opaquebackground
            ? std::array<float, 2> { 0.0f, 0.0f }
            : std::array<float, 2> { -layout.visible_display_offset[0], -layout.visible_display_offset[1] };
    }
};

} // namespace wallpaper
