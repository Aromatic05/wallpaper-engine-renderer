#pragma once

#include "../OutputTarget.hpp"
#include "../OutputTargetBinding.hpp"
#include "../VulkanOutputInit.hpp"

#include <memory>

namespace wallpaper
{
// OutputTargetBinding for the CEF-based web backend. Backend-specific render-plan binding remains
// separate, while SHM and DMA-BUF frames are acquired through OutputTargetBinding's public texture
// contract just like scene and video outputs.
class WebOutputBinding : public OutputTargetBinding {
public:
    explicit WebOutputBinding(RenderInitInfo renderInitInfo);

    OutputTargetBindingKind kind() const override;
    const RenderInitInfo&   renderInitInfo() const;
    void                    attachSwapchain(ExSwapchain* swapchain);

private:
    RenderInitInfo m_renderInitInfo;
};

std::shared_ptr<WebOutputBinding> MakeWebOutputBinding(const RenderInitInfo& renderInitInfo);
OutputTarget                      MakeWebOutputTarget(const std::shared_ptr<WebOutputBinding>& binding);
OutputTarget                      MakeWebOutputTarget(const RenderInitInfo& renderInitInfo);

inline WebOutputBinding::WebOutputBinding(RenderInitInfo renderInitInfo)
    : m_renderInitInfo(std::move(renderInitInfo)) {}

inline OutputTargetBindingKind WebOutputBinding::kind() const {
    return OutputTargetBindingKind::WebRenderTarget;
}

inline const RenderInitInfo& WebOutputBinding::renderInitInfo() const {
    return m_renderInitInfo;
}

inline void WebOutputBinding::attachSwapchain(ExSwapchain* swapchain) {
    attachTextureSwapchain(swapchain);
}


inline std::shared_ptr<WebOutputBinding> MakeWebOutputBinding(const RenderInitInfo& renderInitInfo) {
    return std::make_shared<WebOutputBinding>(renderInitInfo);
}

inline OutputTarget MakeWebOutputTarget(const std::shared_ptr<WebOutputBinding>& binding) {
    OutputTarget target;
    target.type    = binding->renderInitInfo().offscreen ? OutputTargetType::Offscreen
                                                         : OutputTargetType::Surface;
    target.binding = binding;
    target.width   = binding->renderInitInfo().width;
    target.height  = binding->renderInitInfo().height;
    return target;
}

inline OutputTarget MakeWebOutputTarget(const RenderInitInfo& renderInitInfo) {
    return MakeWebOutputTarget(MakeWebOutputBinding(renderInitInfo));
}
} // namespace wallpaper
