#pragma once

#include "common/result/Result.hpp"
#include "wallpaper/web/WebBrowserHost.hpp"

#include <memory>

namespace wallpaper
{
Result<std::shared_ptr<WebBrowserHost>> CreateWebBrowserHostRuntime();
}
