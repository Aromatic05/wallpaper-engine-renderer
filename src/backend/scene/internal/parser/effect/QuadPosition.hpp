#pragma once

#include <string_view>
#include <vector>

namespace wallpaper
{
struct WPShaderInfo;
namespace wpscene
{
class WPMaterial;
}

bool UsesEffectQuadPositionSpace(const wpscene::WPMaterial& material);
bool IsShaderPositionUniform(const WPShaderInfo& shader_info, std::string_view uniform_name);
std::vector<float> NormalizeEffectPositionValue(std::vector<float> value);

} // namespace wallpaper
