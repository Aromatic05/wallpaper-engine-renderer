#pragma once

#include "RenderPlan.hpp"
#include "Result.hpp"

namespace wallpaper
{
class OutputSource {
public:
    virtual ~OutputSource() = default;

    // The current public backend contract exposes only a render plan. Texture and native-surface
    // producers require explicit ownership, synchronization, and lifetime contracts and must not be
    // advertised before those contracts exist.
    virtual Result<RenderPlanPtr> renderPlan() const = 0;
};
} // namespace wallpaper
