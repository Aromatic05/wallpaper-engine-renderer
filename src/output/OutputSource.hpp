#pragma once

#include "common/result/Result.hpp"
#include "output/OutputTarget.hpp"
#include "output/RenderPlan.hpp"

namespace wallpaper
{
enum class OutputSourceType
{
    RenderPlan,
    Texture,
    Surface
};

class OutputSource {
public:
    virtual ~OutputSource() = default;

    virtual OutputSourceType type() const = 0;

    virtual Result<RenderPlanPtr> renderPlan() const {
        return Result<RenderPlanPtr>::failure(ResultCode::NotSupported,
                                              "output source does not expose a render plan");
    }
};
} // namespace wallpaper
