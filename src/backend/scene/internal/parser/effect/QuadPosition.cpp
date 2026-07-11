#include "QuadPosition.hpp"

#include "parser/WPShaderParser.hpp"
#include "wpscene/WPMaterial.h"

namespace wallpaper
{

bool UsesEffectQuadPositionSpace(const wpscene::WPMaterial& material) {
    if (material.shader != "effects/spin" && material.shader != "effects/transform") return false;
    const auto mode_it = material.combos.find("MODE");
    return mode_it != material.combos.end() && mode_it->second == 1;
}

bool IsShaderPositionUniform(const WPShaderInfo& shader_info, std::string_view uniform_name) {
    return shader_info.positionUniforms.count(std::string(uniform_name)) != 0;
}

std::vector<float> NormalizeEffectPositionValue(std::vector<float> value) {
    if (value.size() >= 2) {
        value[0] = value[0] * 2.0f - 1.0f;
        value[1] = value[1] * 2.0f - 1.0f;
    }
    return value;
}

} // namespace wallpaper
