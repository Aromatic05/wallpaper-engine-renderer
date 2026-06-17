#pragma once

#include "common/result/Result.hpp"
#include "output/OutputSource.hpp"
#include "output/OutputTarget.hpp"
#include "runtime/backend/BackendCapabilities.hpp"

#include <string>
#include <string_view>

namespace wallpaper
{
class OutputController {
public:
    Result<void> bind(const OutputTarget&        target,
                      OutputSource&              source,
                      const BackendCapabilities& capabilities) {
        auto validationResult = validate(target, source, capabilities);
        if (! validationResult) {
            return validationResult;
        }

        m_target = target;
        return source.bind(target);
    }

    const OutputTarget& target() const { return m_target; }

private:
    static Result<void> validate(const OutputTarget&        target,
                                 const OutputSource&        source,
                                 const BackendCapabilities& capabilities) {
        if (! target.valid()) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "output target binding is null");
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

    static Result<void> unsupportedBinding(std::string_view sourceType, OutputTargetType targetType) {
        return Result<void>::failure(ResultCode::NotSupported,
                                     "backend does not support binding " + std::string(sourceType)
                                         + " output to " + targetTypeName(targetType) + " target");
    }

    static std::string targetTypeName(OutputTargetType type) {
        switch (type) {
        case OutputTargetType::Surface: return "surface";
        case OutputTargetType::Offscreen: return "offscreen";
        }

        return "unknown";
    }

    OutputTarget m_target {};
};
} // namespace wallpaper
