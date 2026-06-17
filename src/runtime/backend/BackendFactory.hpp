#pragma once

#include "common/result/Result.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/session/WallpaperTypes.hpp"

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
