#include "output/OutputController.hpp"

namespace wallpaper
{
Result<void> OutputController::bind(const OutputTarget& target, OutputSource& source) {
    auto planResult = source.renderPlan();
    if (! planResult) return Result<void>(planResult.error());
    return bind(target, planResult.value());
}

Result<void> OutputController::bind(const OutputTarget& target,
                                    const RenderPlanPtr& resolvedPlan) {
    auto validationResult = validate(target);
    if (! validationResult) return validationResult;
    if (! resolvedPlan) {
        return Result<void>::failure(ResultCode::InvalidState,
                                     "render plan source returned a null plan");
    }

    auto result = bindRenderPlan(target, resolvedPlan);
    if (! result) return result;

    m_target                  = target;
    m_boundRenderPlanRevision = resolvedPlan->revision();
    m_boundRenderPlan         = resolvedPlan;
    return Result<void>::success();
}

Result<void> OutputController::validate(const OutputTarget& target) {
    if (! target.valid()) {
        return Result<void>::failure(ResultCode::InvalidArgument, "output target binding is null");
    }
    return Result<void>::success();
}

Result<void> OutputController::bindRenderPlan(const OutputTarget& target, const RenderPlanPtr& plan) {
    if (plan->requiredBindingKind() != target.binding->kind()) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "render plan binding kind does not match output target binding");
    }
    return plan->bindOutput(target);
}
} // namespace wallpaper
