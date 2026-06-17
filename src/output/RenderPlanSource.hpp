#pragma once

#include "output/OutputSource.hpp"

namespace wallpaper
{
class RenderPlanSource : public OutputSource {
public:
    OutputSourceType type() const final { return OutputSourceType::RenderPlan; }
};
} // namespace wallpaper
