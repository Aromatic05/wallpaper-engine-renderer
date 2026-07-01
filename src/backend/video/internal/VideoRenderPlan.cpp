#include "backend/video/internal/VideoRenderPlan.hpp"

namespace wallpaper
{
VideoRenderPlan::VideoRenderPlan(BindFn bind)
    : m_bind(std::move(bind)) {}

OutputTargetBindingKind VideoRenderPlan::requiredBindingKind() const {
    return OutputTargetBindingKind::VideoRenderTarget;
}

std::uint64_t VideoRenderPlan::revision() const { return m_revision; }

Result<void> VideoRenderPlan::bindOutput(const OutputTarget& target) {
    if (! m_bind) {
        return Result<void>::failure(ResultCode::InvalidState,
                                     "video render plan has no bind function installed");
    }
    auto result = m_bind(target);
    if (result) ++m_revision;
    return result;
}

VideoOutputSource::VideoOutputSource(std::shared_ptr<VideoRenderPlan> plan)
    : m_plan(std::move(plan)) {}

Result<RenderPlanPtr> VideoOutputSource::currentRenderPlan() const {
    return Result<RenderPlanPtr>::success(m_plan);
}
} // namespace wallpaper
