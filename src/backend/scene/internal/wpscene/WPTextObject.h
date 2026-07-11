#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "settings/WPUserProperties.hpp"
#include "wpscene/WPEffect.h"

namespace wallpaper
{

namespace fs
{
class VFS;
}

namespace wpscene
{

struct WPTextObject {
    int32_t                    id { 0 };
    std::string                name;
    bool                       locktransforms { false };
    bool                       muteineditor { false };
    bool                       nointerpolation { false };
    std::vector<int32_t>       dependencies;
    nlohmann::json             instance;
    std::array<float, 3>       origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>       scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3>       angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2>       size { 0.0f, 0.0f };
    std::array<float, 2>       parallaxDepth { 0.0f, 0.0f };
    std::array<float, 3>       color { 1.0f, 1.0f, 1.0f };
    std::array<float, 3>       backgroundcolor { 0.0f, 0.0f, 0.0f };
    float                      alpha { 1.0f };
    float                      backgroundbrightness { 1.0f };
    float                      pointsize { 32.0f };
    float                      maxwidth { 0.0f };
    bool                       size_explicit { false };
    bool                       visible { true };
    VisibleBinding             visible_binding;
    bool                       has_visible_script { false };
    bool                       has_dynamic_layout_script { false };
    bool                       opaquebackground { false };
    bool                       blockalign { false };
    bool                       limitrows { false };
    bool                       limituseellipsis { false };
    bool                       limitwidth { false };
    bool                       copybackground { false };
    int32_t                    maxrows { 1 };
    int32_t                    padding { 0 };
    std::array<int32_t, 4>     padding_edges { 0, 0, 0, 0 }; // top, right, bottom, left
    int32_t                    parent { 0 };
    std::string                attachment;
    std::string                text;
    std::string                font;
    std::string                horizontalalign { "left" };
    std::string                verticalalign { "top" };
    // Wallpaper Engine omits the text anchor field when it uses the default "none" contract.
    // Treating missing anchors as "center" changes newer unpacked scenes into explicit
    // screen-centered text and shifts left/top authored labels.
    std::string                anchor { "none" };
    std::string                depthtest { "disabled" };
    // Text effects share the neutral wallpaper effect model with image layers, but the text object
    // no longer includes WPImageObject just to reference that effect chain.
    std::vector<WPImageEffect> effects;

    bool FromJson(const nlohmann::json& json, fs::VFS& vfs);
};

} // namespace wpscene
} // namespace wallpaper
