#include "FinalComposite.hpp"

#include "parser/WPShaderParser.hpp"
#include "wpscene/WPMaterial.h"

#include <algorithm>

namespace wallpaper
{

bool IsLayerCompositeShader(std::string_view shader) {
    return shader == "genericimage" || shader == "genericimage2" ||
           shader == "genericimage3" || shader == "genericimage4" ||
           shader == "passthrough";
}

bool CanCompositeFinalEffectMaterial(const wpscene::WPMaterial& material,
                                     const WPShaderInfo&         shader_info) {
    if (IsLayerCompositeShader(material.shader) || material.shader == "effects/transform" ||
        material.shader == "effects/scroll" || material.shader == "effects/perspective") {
        return true;
    }

    const bool samples_previous = std::any_of(
        shader_info.textureMaterials.begin(),
        shader_info.textureMaterials.end(),
        [](const auto& entry) { return entry.second == "previous"; });

    // A transparent filter that explicitly samples the current `previous` image already owns the
    // source-over composition in its shader. Letting it write the visible target directly preserves
    // that authored RGB/alpha result; ordinary filters are kept private and published by the neutral
    // final composite instead.
    return shader_info.combos.count("TRANSPARENCY") != 0 && samples_previous;
}

} // namespace wallpaper
