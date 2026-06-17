#pragma once

#include "../../output/VulkanOutputInit.hpp"
#include "../../output/OutputTarget.hpp"
#include "../../output/OutputTargetBinding.hpp"
#include "../../output/swapchain/ExSwapchain.hpp"

#include <memory>

namespace wallpaper
{
class WESceneOutputBinding : public OutputTargetBinding {
public:
    explicit WESceneOutputBinding(RenderInitInfo renderInitInfo);

    OutputTargetBindingKind kind() const override;
    const RenderInitInfo& renderInitInfo() const;
    void                  attachSwapchain(ExSwapchain* swapchain);
    ExSwapchain*          swapchain() const;

private:
    RenderInitInfo m_renderInitInfo;
    ExSwapchain*   m_swapchain { nullptr };
};

std::shared_ptr<WESceneOutputBinding> MakeWESceneOutputBinding(const RenderInitInfo& renderInitInfo);
OutputTarget MakeWESceneOutputTarget(const std::shared_ptr<WESceneOutputBinding>& binding);
OutputTarget MakeWESceneOutputTarget(const RenderInitInfo& renderInitInfo);
} // namespace wallpaper
