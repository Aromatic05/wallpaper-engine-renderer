#include "Fallback.hpp"

#include "wpscene/WPImageObject.h"

namespace wallpaper
{

bool CanUseImageAsSystemMediaFallback(const wpscene::WPImageObject& image) {
    if (!image.puppet.empty()) return false;
    if (image.fullscreen || image.config.passthrough) return false;
    return image.effects.empty();
}

} // namespace wallpaper
