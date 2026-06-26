#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"

#include "backend/web/internal/cef/OsrRenderHandler.hpp"

namespace wallpaper
{
// CefClient + life-span / load / display handler glue. Holds the
// verbatim user-properties JSON (already serialised by LoadWebManifest
// — see include/wallpaper/web/WebTypes.hpp) and injects it into the
// page's wallpaperPropertyListener.applyUserProperties hook on first
// OnLoadEnd. Routes CefConsoleMessage through stderr for diagnostics.
class ClientHandler : public CefClient,
                      public CefLifeSpanHandler,
                      public CefLoadHandler,
                      public CefDisplayHandler {
public:
    ClientHandler(std::string user_props_json, bool has_user_props,
                  CefRefPtr<OsrRenderHandler> render_handler);

    void SetCloseCallback(std::function<void()> cb);

    CefRefPtr<CefBrowser> GetBrowser() const { return browser_; }

    // CefClient.
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler>     GetLoadHandler()     override { return this; }
    CefRefPtr<CefDisplayHandler>  GetDisplayHandler()  override { return this; }
    CefRefPtr<CefRenderHandler>   GetRenderHandler()   override { return render_handler_; }

    // CefLifeSpanHandler.
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    // CefLoadHandler.
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override;

    // CefDisplayHandler.
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                          const CefString& message, const CefString& source, int line) override;

private:
    std::string              user_props_json_;
    bool                     has_user_props_ { false };
    CefRefPtr<OsrRenderHandler> render_handler_;
    CefRefPtr<CefBrowser>    browser_;
    std::function<void()>    close_cb_;
    std::atomic<bool>        property_injected_ { false };

    IMPLEMENT_REFCOUNTING(ClientHandler);
    DISALLOW_COPY_AND_ASSIGN(ClientHandler);
};
} // namespace wallpaper
