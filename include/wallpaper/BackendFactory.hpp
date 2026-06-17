#pragma once

#include "BackendContext.hpp"
#include "Result.hpp"
#include "WallpaperTypes.hpp"

#include <memory>

namespace wallpaper
{
class ContentBackend;

class BackendFactory {
public:
    virtual ~BackendFactory() = default;

    virtual Result<std::unique_ptr<ContentBackend>> create(BackendType type,
                                                           const BackendContext& context) = 0;
};
} // namespace wallpaper
