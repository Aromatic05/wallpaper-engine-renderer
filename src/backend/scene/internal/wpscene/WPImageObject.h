#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "resources/WPJson.hpp"
#include "settings/WPUserProperties.hpp"
#include "WPMaterial.h"
#include "animation/WPPuppet.hpp"
#include "wpscene/WPEffect.h"

namespace wallpaper
{
namespace fs
{
class VFS;
}

namespace wpscene
{

class WPImageObject {
public:
    struct Config {
        bool passthrough { false };
    };
    bool                       FromJson(const nlohmann::json&, fs::VFS&);
    int32_t                    id { 0 };
    std::string                name;
    std::array<float, 3>       origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>       scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3>       angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2>       size { 2.0f, 2.0f };
    std::array<float, 2>       parallaxDepth { 0.0f, 0.0f };
    // Scene JSON omits parallaxDepth for many ordinary image layers, while explicit "0 0" is an
    // authored choice to pin the layer. Keeping this bit separate lets the parser repair omitted
    // root-layer parallax without accidentally moving layers that the wallpaper author set to zero.
    bool                       parallaxDepthAuthored { false };
    std::array<float, 3>       color { 1.0f, 1.0f, 1.0f };
    int32_t                    colorBlendMode { 0 };
    float                      alpha { 1.0f };
    float                      brightness { 1.0f };
    bool                       fullscreen { false };
    bool                       autosize { false };
    // Wallpaper Engine's `models/util/projectlayer.json` stores this marker in the model asset,
    // not on the scene object itself. Keeping it on the parsed image object lets the scene parser
    // distinguish logical framebuffer helper layers from normal drawable image layers.
    bool                       projectlayer { false };
    bool                       nopadding { false };
    bool                       visible { true };
    VisibleBinding             visible_binding;
    std::string                image;
    int32_t                    parent { 0 };
    std::string                attachment;
    std::string                alignment { "center" };
    std::array<float, 2>       effectSourceSize { 0.0f, 0.0f };
    // Some source-less effect layers need framebuffer-sized intermediate targets while their
    // visible output still follows ordinary world-space layer geometry. This is intentionally
    // separate from fullscreen, which also changes final projection and transform semantics.
    bool                       effectSourceScreenBound { false };
    // Optional final writer UV coverage. Effects such as DIRECTDRAW lightshafts can generate
    // visible pixels outside the canonical [0, 1] quad; expanding only the final writer prevents
    // clipping without changing the shader's authored domain.
    bool                       effectFinalTexCoordBoundsEnabled { false };
    std::array<float, 4>       effectFinalTexCoordBounds { 0.0f, 0.0f, 1.0f, 1.0f };
    bool                       copybackground { false };
    WPMaterial                 material;
    std::vector<WPImageEffect> effects;
    Config                     config;

    std::string                                puppet;
    std::vector<WPPuppetLayer::AnimationLayer> puppet_layers;
};

// Image objects now depend on the neutral WPEffect model instead of owning those declarations.
// Text objects include the same effect header directly, which removes the previous text->image
// header dependency while preserving the authored effect JSON shape.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPImageObject, name, origin, angles, scale, size, visible,
                                   material, effects);

} // namespace wpscene
} // namespace wallpaper
