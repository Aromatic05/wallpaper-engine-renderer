#pragma once

#include "wallpaper/OutputTarget.hpp"
#include "wallpaper/OutputTargetBinding.hpp"
#include "wallpaper/VulkanOutputInit.hpp"

#include <memory>

namespace wallpaper
{
class VideoOutputBinding : public OutputTargetBinding {
public:
    explicit VideoOutputBinding(RenderInitInfo renderInitInfo);

    OutputTargetBindingKind kind() const override;
    const RenderInitInfo&   renderInitInfo() const;
    void                    attachSwapchain(ExSwapchain* swapchain);
    ExSwapchain*            swapchain() const;

private:
    RenderInitInfo m_renderInitInfo;
    ExSwapchain*   m_swapchain { nullptr };
};

std::shared_ptr<VideoOutputBinding> MakeVideoOutputBinding(const RenderInitInfo& renderInitInfo);
OutputTarget MakeVideoOutputTarget(const std::shared_ptr<VideoOutputBinding>& binding);
OutputTarget MakeVideoOutputTarget(const RenderInitInfo& renderInitInfo);

inline VideoOutputBinding::VideoOutputBinding(RenderInitInfo renderInitInfo)
    : m_renderInitInfo(std::move(renderInitInfo)) {}

inline OutputTargetBindingKind VideoOutputBinding::kind() const {
    return OutputTargetBindingKind::VideoRenderTarget;
}

inline const RenderInitInfo& VideoOutputBinding::renderInitInfo() const {
    return m_renderInitInfo;
}

inline void VideoOutputBinding::attachSwapchain(ExSwapchain* swapchain) {
    m_swapchain = swapchain;
}

inline ExSwapchain* VideoOutputBinding::swapchain() const { return m_swapchain; }

inline std::shared_ptr<VideoOutputBinding> MakeVideoOutputBinding(
    const RenderInitInfo& renderInitInfo) {
    return std::make_shared<VideoOutputBinding>(renderInitInfo);
}

inline OutputTarget MakeVideoOutputTarget(const std::shared_ptr<VideoOutputBinding>& binding) {
    OutputTarget target;
    target.type = binding->renderInitInfo().offscreen ? OutputTargetType::Offscreen
                                                      : OutputTargetType::Surface;
    target.binding = binding;
    target.width = binding->renderInitInfo().width;
    target.height = binding->renderInitInfo().height;
    return target;
}

inline OutputTarget MakeVideoOutputTarget(const RenderInitInfo& renderInitInfo) {
    return MakeVideoOutputTarget(MakeVideoOutputBinding(renderInitInfo));
}
} // namespace wallpaper
