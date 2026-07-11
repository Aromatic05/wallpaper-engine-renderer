#pragma once

#include <cstdint>

namespace wallpaper
{
namespace wpscene
{
class WPMaterial;
}

struct ImageColorBlendPlan {
    bool apply_to_layer_material { false };
    bool append_final_effect { false };
};

ImageColorBlendPlan ResolveImageColorBlendPlan(int32_t color_blend_mode,
                                               bool    has_authored_effect,
                                               bool    has_animated_puppet_mesh);
void ApplyImageColorBlend(wpscene::WPMaterial& material, int32_t color_blend_mode);
void ApplyImageEffectContext(wpscene::WPMaterial& material, bool copy_background);

} // namespace wallpaper
