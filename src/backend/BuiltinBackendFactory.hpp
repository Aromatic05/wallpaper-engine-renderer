#pragma once

#include "runtime/backend/BackendFactory.hpp"

#include <memory>

namespace wallpaper
{
std::shared_ptr<BackendFactory> CreateBuiltinBackendFactory();
} // namespace wallpaper
