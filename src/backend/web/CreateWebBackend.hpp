#pragma once

#include "common/result/Result.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/ContentBackend.hpp"
#include "wallpaper/web/WebEngineServices.hpp"

#include <memory>

namespace wallpaper
{
Result<std::unique_ptr<ContentBackend>> CreateWebBackend(
    const BackendContext& context,
    std::shared_ptr<WebEngineServices> services = {});
} // namespace wallpaper
