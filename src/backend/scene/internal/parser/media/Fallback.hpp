#pragma once

namespace wallpaper::wpscene
{
class WPImageObject;
}

namespace wallpaper
{

bool CanUseImageAsSystemMediaFallback(const wpscene::WPImageObject& image);

} // namespace wallpaper
