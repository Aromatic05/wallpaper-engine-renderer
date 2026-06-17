#pragma once

#include "backend/scene/compatibility/WESceneRenderInit.hpp"
#include "Swapchain/ExSwapchain.hpp"
#include "output/OutputTarget.hpp"

#include <memory>

namespace wallpaper
{
class WESceneOutputBinding {
public:
    explicit WESceneOutputBinding(RenderInitInfo renderInitInfo);

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
