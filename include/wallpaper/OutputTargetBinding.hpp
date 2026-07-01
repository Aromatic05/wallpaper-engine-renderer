#pragma once

#include <memory>

namespace wallpaper
{
enum class OutputTargetBindingKind
{
    Surface,
    Offscreen,
    VulkanRenderTarget,
    VideoRenderTarget,
    // CEF/Chromium-driven web wallpapers. The binding owns the
    // swapchain the BrowserHost writes OnAcceleratedPaint DMA-BUFs
    // into. Consumers read frames via the same dynamic_cast path
    // they use for VulkanRenderTarget.
    WebRenderTarget,
};

class OutputTargetBinding {
public:
    virtual ~OutputTargetBinding() = default;

    virtual OutputTargetBindingKind kind() const = 0;
};

using OutputTargetBindingPtr = std::shared_ptr<OutputTargetBinding>;
} // namespace wallpaper
