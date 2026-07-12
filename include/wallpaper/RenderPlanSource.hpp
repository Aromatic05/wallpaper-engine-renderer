#pragma once

#include "OutputSource.hpp"

namespace wallpaper
{
class RenderPlanSource : public OutputSource {
public:
    Result<RenderPlanPtr> renderPlan() const final { return currentRenderPlan(); }

protected:
    virtual Result<RenderPlanPtr> currentRenderPlan() const = 0;
};
} // namespace wallpaper
