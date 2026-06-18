#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SceneMesh.h"
#include "scene/Image.hpp"
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
    std::array<float, 2> logical_size { 0.0f, 0.0f };
    std::array<float, 2> logical_source_size { 0.0f, 0.0f };
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

    wpscene::WPTextObject object;
    TextLayoutResult      layout;
    TextSourceBridge      bridge;
    std::shared_ptr<SceneMesh> background_mesh;
    std::vector<GlyphPageRenderable> glyph_pages;
    uint32_t              atlas_version { 0 };

    [[nodiscard]] std::array<float, 2> VisibleDisplaySize() const {
        return layout.visible_display_size;
    }
    [[nodiscard]] std::array<float, 2> VisibleSourceSize() const {
        return layout.visible_source_size;
    }
    [[nodiscard]] std::array<float, 2> VisibleDisplayOffset() const {
        return layout.visible_display_offset;
    }
    [[nodiscard]] std::array<float, 2> BackgroundLocalOffset() const {
        return object.opaquebackground
                   ? std::array<float, 2> { 0.0f, 0.0f }
                   : std::array<float, 2> { -layout.visible_display_offset[0],
                                            -layout.visible_display_offset[1] };
    }
};

} // namespace wallpaper
