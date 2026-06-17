#pragma once

#include "../OutputTarget.hpp"
#include "../OutputTargetBinding.hpp"
#include "../VulkanOutputInit.hpp"

#include <memory>

namespace wallpaper
{
class WESceneOutputBinding : public OutputTargetBinding {
public:
    explicit WESceneOutputBinding(RenderInitInfo renderInitInfo);

    OutputTargetBindingKind kind() const override;
    const RenderInitInfo&   renderInitInfo() const;
    void                    attachSwapchain(ExSwapchain* swapchain);
    ExSwapchain*            swapchain() const;

private:
    RenderInitInfo m_renderInitInfo;
    ExSwapchain*   m_swapchain { nullptr };
};

std::shared_ptr<WESceneOutputBinding> MakeWESceneOutputBinding(const RenderInitInfo& renderInitInfo);
OutputTarget MakeWESceneOutputTarget(const std::shared_ptr<WESceneOutputBinding>& binding);
OutputTarget MakeWESceneOutputTarget(const RenderInitInfo& renderInitInfo);

inline WESceneOutputBinding::WESceneOutputBinding(RenderInitInfo renderInitInfo)
    : m_renderInitInfo(std::move(renderInitInfo)) {}

inline OutputTargetBindingKind WESceneOutputBinding::kind() const {
    return OutputTargetBindingKind::WESceneVulkan;
}

inline const RenderInitInfo& WESceneOutputBinding::renderInitInfo() const {
    return m_renderInitInfo;
}

inline void WESceneOutputBinding::attachSwapchain(ExSwapchain* swapchain) {
    m_swapchain = swapchain;
}

inline ExSwapchain* WESceneOutputBinding::swapchain() const { return m_swapchain; }

inline std::shared_ptr<WESceneOutputBinding> MakeWESceneOutputBinding(
    const RenderInitInfo& renderInitInfo) {
    return std::make_shared<WESceneOutputBinding>(renderInitInfo);
}

inline OutputTarget MakeWESceneOutputTarget(const std::shared_ptr<WESceneOutputBinding>& binding) {
    OutputTarget target;
    target.type    = binding->renderInitInfo().offscreen ? OutputTargetType::Offscreen
                                                         : OutputTargetType::Surface;
    target.binding = binding;
    target.width   = binding->renderInitInfo().width;
    target.height  = binding->renderInitInfo().height;
    return target;
}

inline OutputTarget MakeWESceneOutputTarget(const RenderInitInfo& renderInitInfo) {
    return MakeWESceneOutputTarget(MakeWESceneOutputBinding(renderInitInfo));
}
} // namespace wallpaper
