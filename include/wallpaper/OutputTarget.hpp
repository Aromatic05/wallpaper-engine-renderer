#pragma once

#include "OutputTargetBinding.hpp"

#include <cstdint>

namespace wallpaper
{
enum class OutputTargetType
{
    Surface,
    Offscreen
};

struct OutputTarget {
    OutputTargetType       type { OutputTargetType::Surface };
    OutputTargetBindingPtr binding;
    std::uint16_t          width { 0 };
    std::uint16_t          height { 0 };

    bool valid() const { return binding != nullptr; }
};
} // namespace wallpaper
