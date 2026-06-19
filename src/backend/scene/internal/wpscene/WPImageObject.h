#pragma once
#include "resources/WPJson.hpp"
#include <nlohmann/json.hpp>
#include "WPMaterial.h"
#include "wpscene/WPEffect.h"
#include <vector>
#include "animation/WPPuppet.hpp"
#include <string>

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
    std::array<float, 3>       color { 1.0f, 1.0f, 1.0f };
    int32_t                    colorBlendMode { 0 };
    float                      alpha { 1.0f };
    float                      brightness { 1.0f };
    bool                       fullscreen { false };
    bool                       nopadding { false };
    bool                       visible { true };
    std::string                image;
    std::string                alignment { "center" };
    WPMaterial                 material;
    std::vector<WPImageEffect> effects;
    Config                     config;

    std::string                                puppet;
    std::vector<WPPuppetLayer::AnimationLayer> puppet_layers;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPImageObject, name, origin, angles, scale, size, visible,
                                   material, effects);

} // namespace wpscene
} // namespace wallpaper
