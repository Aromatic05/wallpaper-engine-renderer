#include "backend/web/internal/WebRenderPlan.hpp"

namespace wallpaper
{
WebRenderPlan::WebRenderPlan(BindFn bind): m_bind(std::move(bind)) {}

OutputTargetBindingKind WebRenderPlan::requiredBindingKind() const {
    return OutputTargetBindingKind::WebRenderTarget;
}

std::uint64_t WebRenderPlan::revision() const { return m_revision; }

Result<void> WebRenderPlan::bindOutput(const OutputTarget& target) {
    if (! m_bind) {
        return Result<void>::failure(ResultCode::InvalidState,
                                     "web render plan has no bind function installed");
    }
    auto r = m_bind(target);
    if (r) {
        m_revision++;
    }
    return r;
}

WebOutputSource::WebOutputSource(std::shared_ptr<WebRenderPlan> plan)
    : m_plan(std::move(plan)) {}

Result<RenderPlanPtr> WebOutputSource::currentRenderPlan() const {
    return Result<RenderPlanPtr>::success(m_plan);
}
} // namespace wallpaper
