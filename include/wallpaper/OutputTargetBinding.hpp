#pragma once

#include "Result.hpp"
#include "TextureOutput.hpp"
#include "swapchain/ExSwapchain.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
namespace wallpaper
{

enum class OutputTargetBindingKind
{
    Surface,
    Offscreen,
    VulkanRenderTarget,
    VideoRenderTarget,
    // CEF/Chromium-driven web wallpapers use the same public texture-frame acquisition contract as
    // scene and video bindings; only their render-plan binding kind remains backend-specific.
    WebRenderTarget,
};

class OutputTargetBinding {
public:
    virtual ~OutputTargetBinding() = default;

    virtual OutputTargetBindingKind kind() const = 0;
    Result<TextureFrame> acquireTexture();

protected:
    void attachTextureSwapchain(ExSwapchain* swapchain) {
        std::scoped_lock lock(m_textureSwapchainMutex);
        m_textureSwapchain = swapchain;
    }

private:
    mutable std::mutex       m_textureSwapchainMutex;
    ExSwapchain*             m_textureSwapchain { nullptr };
    std::atomic<std::uint64_t> m_textureRevision { 0 };
};

using OutputTargetBindingPtr = std::shared_ptr<OutputTargetBinding>;
} // namespace wallpaper
