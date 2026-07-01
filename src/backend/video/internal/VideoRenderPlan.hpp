#pragma once

#include "output/RenderPlan.hpp"
#include "output/RenderPlanSource.hpp"

#include <functional>
#include <memory>

namespace wallpaper
{
class VideoRenderPlan final : public RenderPlan {
public:
    using BindFn = std::function<Result<void>(const OutputTarget&)>;

    explicit VideoRenderPlan(BindFn bind = {});

    OutputTargetBindingKind requiredBindingKind() const override;
    std::uint64_t           revision() const override;
    Result<void>            bindOutput(const OutputTarget& target) override;

private:
    BindFn         m_bind;
    std::uint64_t  m_revision { 0 };
};

class VideoOutputSource final : public RenderPlanSource {
public:
    explicit VideoOutputSource(std::shared_ptr<VideoRenderPlan> plan);

protected:
    Result<RenderPlanPtr> currentRenderPlan() const override;

private:
    std::shared_ptr<VideoRenderPlan> m_plan;
};
} // namespace wallpaper
