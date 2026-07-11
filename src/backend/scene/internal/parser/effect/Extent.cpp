#include "Extent.hpp"

#include "scene/Scene.h"
#include "scene/SceneCamera.h"
#include "wpscene/WPImageObject.h"

#include <algorithm>
#include <cmath>

namespace wallpaper
{

int32_t NonZeroRenderTargetDimension(float value) {
    if (!std::isfinite(value) || value < 1.0f) return 1;
    return std::max<int32_t>(1, static_cast<int32_t>(value));
}

std::array<int32_t, 2> NonZeroRenderTargetExtent(float width, float height) {
    return { NonZeroRenderTargetDimension(width), NonZeroRenderTargetDimension(height) };
}

std::array<float, 2> ResolveImageEffectTargetSize(
    const Scene* scene,
    const wpscene::WPImageObject& image,
    std::array<float, 2> authored_source_size) {
    if ((image.fullscreen || image.config.passthrough) && scene != nullptr &&
        scene->activeCamera != nullptr) {
        return {
            static_cast<float>(scene->activeCamera->Width()),
            static_cast<float>(scene->activeCamera->Height()),
        };
    }
    return authored_source_size;
}

} // namespace wallpaper
