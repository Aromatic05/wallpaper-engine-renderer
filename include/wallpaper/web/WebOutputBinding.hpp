#pragma once

#include "../OutputTarget.hpp"
#include "../OutputTargetBinding.hpp"
#include "../VulkanOutputInit.hpp"

#include <memory>

namespace wallpaper
{
// OutputTargetBinding for the CEF-based web backend. Mirrors
// WESceneOutputBinding's surface API (RenderInitInfo + swapchain
// attachment) but advertises OutputTargetBindingKind::WebRenderTarget
// so the OutputController validate path picks the right kind check
// and the C ABI's central dynamic_cast routes eatFrame() to the
// web's swapchain rather than the scene's.
//
// `swapchain()` lives on the binding rather than the base class so
// the scene and web paths stay decoupled; the ABI does the
// dynamic_cast at the single point that needs to read frames.
class WebOutputBinding : public OutputTargetBinding {
public:
    explicit WebOutputBinding(RenderInitInfo renderInitInfo);

    OutputTargetBindingKind kind() const override;
    const RenderInitInfo&   renderInitInfo() const;
    void                    attachSwapchain(ExSwapchain* swapchain);
    ExSwapchain*            swapchain() const;

private:
    RenderInitInfo m_renderInitInfo;
    ExSwapchain*   m_swapchain { nullptr };
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
    m_swapchain = swapchain;
}

inline ExSwapchain* WebOutputBinding::swapchain() const { return m_swapchain; }

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
