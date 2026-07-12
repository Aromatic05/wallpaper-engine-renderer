#include "ColorBlend.hpp"

#include "wpscene/WPMaterial.h"
#include "SpecTexs.hpp"

namespace wallpaper
{

ImageColorBlendPlan ResolveImageColorBlendPlan(int32_t color_blend_mode,
                                               bool    has_authored_effect,
                                               bool    has_animated_puppet_mesh) {
    if (color_blend_mode == 0) return {};

    const bool layer_material_is_final = !has_authored_effect || has_animated_puppet_mesh;
    return {
        .apply_to_layer_material = layer_material_is_final,
        .append_final_effect     = !layer_material_is_final,
    };
}

void ApplyImageColorBlend(wpscene::WPMaterial& material, int32_t color_blend_mode) {
    if (color_blend_mode == 0) return;
    material.combos[std::string(WE_CB_BLENDMODE)] = color_blend_mode;
}

void ApplyImageEffectContext(wpscene::WPMaterial& material, bool copy_background) {
    if (copy_background) material.combos["COPYBG"] = 1;
}

} // namespace wallpaper
