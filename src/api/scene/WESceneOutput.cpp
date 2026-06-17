#include "api/scene/WESceneOutput.hpp"

namespace wallpaper
{
WESceneOutputBinding::WESceneOutputBinding(RenderInitInfo renderInitInfo)
    : m_renderInitInfo(std::move(renderInitInfo)) {}

OutputTargetBindingKind WESceneOutputBinding::kind() const { return OutputTargetBindingKind::WESceneVulkan; }

const RenderInitInfo& WESceneOutputBinding::renderInitInfo() const { return m_renderInitInfo; }

void WESceneOutputBinding::attachSwapchain(ExSwapchain* swapchain) { m_swapchain = swapchain; }

ExSwapchain* WESceneOutputBinding::swapchain() const { return m_swapchain; }

std::shared_ptr<WESceneOutputBinding> MakeWESceneOutputBinding(const RenderInitInfo& renderInitInfo) {
    return std::make_shared<WESceneOutputBinding>(renderInitInfo);
}

OutputTarget MakeWESceneOutputTarget(const std::shared_ptr<WESceneOutputBinding>& binding) {
    OutputTarget target;
    target.type    = binding->renderInitInfo().offscreen ? OutputTargetType::Offscreen
                                                         : OutputTargetType::Surface;
    target.binding = binding;
    target.width   = binding->renderInitInfo().width;
    target.height  = binding->renderInitInfo().height;
    return target;
}

OutputTarget MakeWESceneOutputTarget(const RenderInitInfo& renderInitInfo) {
    return MakeWESceneOutputTarget(MakeWESceneOutputBinding(renderInitInfo));
}
} // namespace wallpaper
