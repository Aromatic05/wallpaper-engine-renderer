#pragma once

#include "common/result/Result.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/ContentBackend.hpp"

#include <memory>

namespace wallpaper
{
Result<std::unique_ptr<ContentBackend>> CreateWESceneBackend(const BackendContext& context);
} // namespace wallpaper
