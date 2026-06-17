#pragma once

#include "output/OutputSource.hpp"

namespace wallpaper
{
class SurfaceSource : public OutputSource {
public:
    OutputSourceType type() const final { return OutputSourceType::Surface; }
};
} // namespace wallpaper
