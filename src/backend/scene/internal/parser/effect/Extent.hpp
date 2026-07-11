#pragma once

#include <array>
#include <cstdint>

namespace wallpaper
{
class Scene;
namespace wpscene
{
class WPImageObject;
}

int32_t NonZeroRenderTargetDimension(float value);
std::array<int32_t, 2> NonZeroRenderTargetExtent(float width, float height);
std::array<float, 2> ResolveImageEffectTargetSize(
    const Scene* scene,
    const wpscene::WPImageObject& image,
    std::array<float, 2> authored_source_size);

} // namespace wallpaper
