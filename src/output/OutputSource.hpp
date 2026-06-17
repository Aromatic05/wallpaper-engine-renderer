#pragma once

#include "common/result/Result.hpp"
#include "output/OutputTarget.hpp"

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

    virtual Result<void> bind(const OutputTarget&) {
        return Result<void>::failure(ResultCode::NotSupported, "output source cannot bind target");
    }
};
} // namespace wallpaper
