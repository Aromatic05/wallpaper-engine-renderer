#pragma once

#include "output/OutputSource.hpp"

namespace wallpaper
{
class TextureSource : public OutputSource {
public:
    OutputSourceType type() const final { return OutputSourceType::Texture; }
};
} // namespace wallpaper
