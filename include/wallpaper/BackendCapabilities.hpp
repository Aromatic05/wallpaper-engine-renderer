#pragma once

namespace wallpaper
{
struct BackendCapabilities {
    bool supportsProperties { true };
    bool supportsInput { false };
    bool supportsRenderPlan { false };
    bool supportsTextureOutput { false };
    bool supportsSurfaceOutput { false };
};
} // namespace wallpaper
