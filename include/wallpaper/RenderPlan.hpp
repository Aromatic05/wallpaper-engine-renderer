#pragma once

#include "OutputTarget.hpp"
#include "Result.hpp"

#include <cstdint>
#include <memory>

namespace wallpaper
{
class RenderPlan {
public:
    virtual ~RenderPlan() = default;

    virtual OutputTargetBindingKind requiredBindingKind() const = 0;
    virtual std::uint64_t revision() const = 0;
    virtual Result<void> bindOutput(const OutputTarget& target) = 0;
};

using RenderPlanPtr = std::shared_ptr<RenderPlan>;
} // namespace wallpaper
