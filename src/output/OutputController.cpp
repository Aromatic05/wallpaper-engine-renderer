#include "output/OutputController.hpp"

#include "api/scene/WESceneOutput.hpp"
#include "api/scene/WESceneRenderPlan.hpp"

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
    auto validationResult = validate(target, source, capabilities);
    if (! validationResult) {
        return validationResult;
    }

    m_target = target;
    switch (source.type()) {
    case OutputSourceType::RenderPlan:
        return bindRenderPlan(target, source);
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
        if (target.binding->kind() != OutputTargetBindingKind::WESceneVulkan) {
            return Result<void>::failure(
                ResultCode::InvalidArgument,
                "render plan output currently requires a WE scene Vulkan target binding");
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

Result<void> OutputController::bindRenderPlan(const OutputTarget& target, const OutputSource& source) {
    auto planResult = source.renderPlan();
    if (! planResult) {
        return Result<void>(planResult.error());
    }

    auto plan = std::dynamic_pointer_cast<WESceneRenderPlan>(planResult.value());
    if (! plan) {
        return Result<void>::failure(ResultCode::NotSupported,
                                     "output controller does not know how to consume this render plan");
    }

    auto binding = std::dynamic_pointer_cast<WESceneOutputBinding>(target.binding);
    if (! binding) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "render plan output target binding is not a WE scene Vulkan binding");
    }

    return plan->prepareOutput(*binding);
}
} // namespace wallpaper
