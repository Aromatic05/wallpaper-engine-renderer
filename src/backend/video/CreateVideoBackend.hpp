#pragma once

#include "runtime/backend/BackendFactory.hpp"

namespace wallpaper
{
Result<std::unique_ptr<ContentBackend>> CreateVideoBackend(const BackendContext& context);
}
