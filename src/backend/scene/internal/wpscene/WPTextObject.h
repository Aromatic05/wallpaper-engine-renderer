#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "WPImageObject.h"

namespace wallpaper
{
namespace fs
{
class VFS;
}

namespace wpscene
{

struct WPTextObject {
    int32_t              id { 0 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> size { 0.0f, 0.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> backgroundcolor { 0.0f, 0.0f, 0.0f };
    float                alpha { 1.0f };
    float                backgroundbrightness { 1.0f };
    float                pointsize { 32.0f };
    float                maxwidth { 0.0f };
    bool                 size_explicit { false };
    bool                 visible { true };
    bool                 has_visible_script { false };
    bool                 has_dynamic_layout_script { false };
    bool                 opaquebackground { false };
    bool                 blockalign { false };
    bool                 limitrows { false };
    bool                 limituseellipsis { false };
    bool                 limitwidth { false };
    int32_t              maxrows { 1 };
    int32_t              padding { 0 };
    std::array<int32_t, 4> padding_edges { 0, 0, 0, 0 };
    int32_t              parent { 0 };
    std::string          attachment;
    std::string          text;
    std::string          font;
    std::string          horizontalalign { "left" };
    std::string          verticalalign { "top" };
    std::string          anchor { "none" };
    std::string          depthtest { "disabled" };
    std::vector<WPImageEffect> effects;

    bool FromJson(const nlohmann::json& json, fs::VFS& vfs);
};

} // namespace wpscene
} // namespace wallpaper
