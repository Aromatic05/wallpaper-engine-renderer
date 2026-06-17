#pragma once

#include <memory>

namespace wallpaper
{
class RenderPlan {
public:
    virtual ~RenderPlan() = default;
};

using RenderPlanPtr = std::shared_ptr<RenderPlan>;
} // namespace wallpaper
