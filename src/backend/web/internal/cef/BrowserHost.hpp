#pragma once

#include "wallpaper/web/WebBrowserHost.hpp"

#include "include/cef_app.h"
#include "include/cef_browser.h"

#include "backend/web/internal/cef/AppHandler.hpp"
#include "backend/web/internal/cef/ClientHandler.hpp"
#include "backend/web/internal/cef/OsrRenderHandler.hpp"

#include <atomic>

namespace wallpaper
{
struct WebBrowserHost::Impl {
    CefRefPtr<AppHandler>       app;
    CefRefPtr<OsrRenderHandler> osr;
    CefRefPtr<ClientHandler>    client;
    AcceleratedPaintCallback    accel_cb;
    std::atomic<bool>           should_exit { false };
    std::atomic<bool>           close_requested { false };
    bool                        initialised { false };
    bool                        runtime_acquired { false };
};
} // namespace wallpaper
