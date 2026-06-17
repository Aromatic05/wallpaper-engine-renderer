#pragma once

#include "HostServices.hpp"

#include <memory>
#include <string>

namespace wallpaper
{
struct BackendContext {
    std::string cachePath;
    std::shared_ptr<HostServices> hostServices;
};
} // namespace wallpaper
