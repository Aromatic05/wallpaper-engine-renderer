#pragma once

#include <span>
#include <string_view>

namespace wallpaper
{
struct WPShaderInfo;
struct WPShaderUnit;
namespace wpscene
{
class WPMaterial;
}

bool IsLegacyAtmosphereMaterial(const wpscene::WPMaterial& material);
void ApplyLegacyAtmosphereUniformAliases(const wpscene::WPMaterial& material,
                                         WPShaderInfo& shader_info);
void ApplyLegacyAtmosphereLightCombo(const wpscene::WPMaterial& material,
                                     WPShaderInfo& shader_info);
void ApplyLegacyAtmosphereShaderCompat(const wpscene::WPMaterial& material,
                                       std::span<WPShaderUnit> units);
bool IsLegacyAtmosphereShadowValue(const wpscene::WPMaterial& material,
                                   std::string_view material_value_name);

} // namespace wallpaper
