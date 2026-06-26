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
    bool                        initialised { false };
    // Stash the original argv from RunOrExitIfHelper; CefInitialize
    // needs the real argv to derive the per-child --type=… /
    // --icu-data-file=… switches it forwards to subprocesses.
    int    saved_argc { 0 };
    char** saved_argv { nullptr };
};
} // namespace wallpaper
