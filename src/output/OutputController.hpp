#pragma once

#include "output/OutputSource.hpp"
#include "output/OutputTarget.hpp"

#include <optional>

namespace wallpaper
{
class OutputController {
public:
    Result<void> bind(const OutputTarget& target, OutputSource& source);
    Result<void> bind(const OutputTarget& target, const RenderPlanPtr& resolvedPlan);

    const OutputTarget&          target() const { return m_target; }
    std::optional<std::uint64_t> boundRenderPlanRevision() const { return m_boundRenderPlanRevision; }
    const RenderPlan*            boundRenderPlan() const { return m_boundRenderPlan.get(); }

private:
    static Result<void> validate(const OutputTarget& target);
    static Result<void> bindRenderPlan(const OutputTarget& target, const RenderPlanPtr& plan);

    OutputTarget                 m_target {};
    std::optional<std::uint64_t> m_boundRenderPlanRevision;
    RenderPlanPtr                m_boundRenderPlan;
};
} // namespace wallpaper
