#pragma once

#include "Result.hpp"
#include "TextureOutput.hpp"
#include "swapchain/ExSwapchain.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
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
    Result<TextureFrame> acquireTexture(std::uint32_t reusableBufferMask = 0);
    void setFrameReadyCallback(std::function<void()> callback);

protected:
    void attachTextureSwapchain(ExSwapchain* swapchain) {
        std::scoped_lock lock(m_textureSwapchainMutex);
        if (m_textureSwapchain == swapchain) return;
        if (m_textureSwapchain) m_textureSwapchain->setOnReady({});
        m_textureSwapchain = swapchain;
        if (m_textureSwapchain) m_textureSwapchain->setOnReady(m_frameReadyCallback);
    }

private:
    mutable std::mutex        m_textureSwapchainMutex;
    ExSwapchain*              m_textureSwapchain { nullptr };
    std::function<void()>     m_frameReadyCallback;
    std::atomic<std::uint64_t> m_textureRevision { 0 };
};

using OutputTargetBindingPtr = std::shared_ptr<OutputTargetBinding>;
} // namespace wallpaper
