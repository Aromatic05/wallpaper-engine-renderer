#pragma once

#include <string_view>

namespace wallpaper
{
struct WPShaderInfo;
namespace wpscene
{
class WPMaterial;
}

bool IsLayerCompositeShader(std::string_view shader);
bool CanCompositeFinalEffectMaterial(const wpscene::WPMaterial& material,
                                     const WPShaderInfo&         shader_info);

} // namespace wallpaper
