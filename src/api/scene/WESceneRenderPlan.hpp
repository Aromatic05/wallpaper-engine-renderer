#pragma once

#include "../../common/result/Result.hpp"
#include "../../output/RenderPlan.hpp"

namespace wallpaper
{
class WESceneOutputBinding;

class WESceneRenderPlan : public RenderPlan {
public:
    ~WESceneRenderPlan() override = default;

    virtual Result<void> prepareOutput(WESceneOutputBinding& binding) = 0;
};
} // namespace wallpaper
