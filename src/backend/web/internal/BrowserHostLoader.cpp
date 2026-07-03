#include "backend/web/internal/BrowserHostLoader.hpp"

namespace wallpaper
{
std::shared_ptr<WebBrowserHost> CreateCefWebBrowserHost();

Result<std::shared_ptr<WebBrowserHost>> CreateWebBrowserHostRuntime() {
    auto host = CreateCefWebBrowserHost();
    if (! host) {
        return Result<std::shared_ptr<WebBrowserHost>>::failure(
            ResultCode::InternalError, "failed to allocate CEF WebBrowserHost");
    }
    return Result<std::shared_ptr<WebBrowserHost>>::success(std::move(host));
}
} // namespace wallpaper
