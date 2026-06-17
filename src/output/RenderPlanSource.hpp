#pragma once

#include "output/OutputSource.hpp"

namespace wallpaper
{
class RenderPlanSource : public OutputSource {
public:
    OutputSourceType type() const final { return OutputSourceType::RenderPlan; }

    Result<RenderPlanPtr> renderPlan() const final { return currentRenderPlan(); }

protected:
    virtual Result<RenderPlanPtr> currentRenderPlan() const = 0;
};
} // namespace wallpaper
