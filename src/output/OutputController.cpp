#include "output/OutputController.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace wallpaper
{
namespace
{
std::string targetTypeName(OutputTargetType type) {
    switch (type) {
    case OutputTargetType::Surface: return "surface";
    case OutputTargetType::Offscreen: return "offscreen";
    }

    return "unknown";
}

Result<void> unsupportedBinding(std::string_view sourceType, OutputTargetType targetType) {
    return Result<void>::failure(ResultCode::NotSupported,
                                 "backend does not support binding " + std::string(sourceType)
                                     + " output to " + targetTypeName(targetType) + " target");
}
} // namespace

Result<void> OutputController::bind(const OutputTarget&        target,
                                    OutputSource&              source,
                                    const BackendCapabilities& capabilities) {
    if (source.type() == OutputSourceType::RenderPlan) {
        auto planResult = source.renderPlan();
        if (! planResult) {
            return Result<void>(planResult.error());
        }
        return bind(target, source, capabilities, planResult.value());
    }

    auto validationResult = validate(target, source, capabilities);
    if (! validationResult) {
        return validationResult;
    }

    switch (source.type()) {
    case OutputSourceType::RenderPlan:
        break;
    case OutputSourceType::Texture:
        return Result<void>::failure(ResultCode::NotSupported,
                                     "texture output controller is not implemented yet");
    case OutputSourceType::Surface:
        return Result<void>::failure(ResultCode::NotSupported,
                                     "surface output controller is not implemented yet");
    }

    return Result<void>::failure(ResultCode::NotSupported, "unknown output source type");
}

Result<void> OutputController::bind(const OutputTarget&        target,
                                    OutputSource&              source,
                                    const BackendCapabilities& capabilities,
                                    const RenderPlanPtr&       resolvedPlan) {
    auto validationResult = validate(target, source, capabilities);
    if (! validationResult) {
        return validationResult;
    }

    switch (source.type()) {
    case OutputSourceType::RenderPlan:
        {
            const auto& plan = resolvedPlan;
            if (! plan) {
                return Result<void>::failure(ResultCode::InvalidState,
                                             "render plan source returned a null plan");
            }

            auto result = bindRenderPlan(target, plan);
            if (! result) {
                return result;
            }

            m_target                  = target;
            m_boundRenderPlanRevision = plan->revision();
            m_boundRenderPlan         = plan;
            return Result<void>::success();
        }
    case OutputSourceType::Texture:
        return Result<void>::failure(ResultCode::NotSupported,
                                     "texture output controller is not implemented yet");
    case OutputSourceType::Surface:
        return Result<void>::failure(ResultCode::NotSupported,
                                     "surface output controller is not implemented yet");
    }

    return Result<void>::failure(ResultCode::NotSupported, "unknown output source type");
}

Result<void> OutputController::validate(const OutputTarget&        target,
                                        const OutputSource&        source,
                                        const BackendCapabilities& capabilities) {
    if (! target.valid()) {
        return Result<void>::failure(ResultCode::InvalidArgument, "output target binding is null");
    }

    switch (source.type()) {
    case OutputSourceType::RenderPlan:
        if (! capabilities.supportsRenderPlan) {
            return unsupportedBinding("render plan", target.type);
        }
        return Result<void>::success();
    case OutputSourceType::Texture:
        if (! capabilities.supportsTextureOutput) {
            return unsupportedBinding("texture", target.type);
        }
        if (target.type != OutputTargetType::Offscreen) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "texture output requires an offscreen output target");
        }
        return Result<void>::success();
    case OutputSourceType::Surface:
        if (! capabilities.supportsSurfaceOutput) {
            return unsupportedBinding("surface", target.type);
        }
        if (target.type != OutputTargetType::Surface) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "surface output requires a surface output target");
        }
        return Result<void>::success();
    }

    return Result<void>::failure(ResultCode::NotSupported, "unknown output source type");
}

std::string OutputController::targetTypeName(OutputTargetType type) {
    switch (type) {
    case OutputTargetType::Surface: return "surface";
    case OutputTargetType::Offscreen: return "offscreen";
    }

    return "unknown";
}

Result<void> OutputController::bindRenderPlan(const OutputTarget& target, const RenderPlanPtr& plan) {
    if (plan->requiredBindingKind() != target.binding->kind()) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "render plan binding kind does not match output target binding");
    }

    return plan->bindOutput(target);
}
} // namespace wallpaper
