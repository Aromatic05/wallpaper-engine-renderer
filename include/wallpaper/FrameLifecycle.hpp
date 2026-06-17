#pragma once

namespace wallpaper
{
struct FrameLifecycle final {
    bool contentStateChanged { false };
    bool outputStateChanged { false };
    bool diagnosticsChanged { false };
    bool frameRequested { false };
};
} // namespace wallpaper
