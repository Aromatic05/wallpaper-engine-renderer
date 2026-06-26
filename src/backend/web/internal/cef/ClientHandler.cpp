#include "backend/web/internal/cef/ClientHandler.hpp"

#include <cstdio>

#include "include/cef_frame.h"

#include "backend/web/internal/cef/UserProperties.hpp"

namespace wallpaper
{
ClientHandler::ClientHandler(std::string user_props_json, bool has_user_props,
                             CefRefPtr<OsrRenderHandler> render_handler)
    : user_props_json_(std::move(user_props_json))
    , has_user_props_(has_user_props)
    , render_handler_(std::move(render_handler)) {}

void ClientHandler::SetCloseCallback(std::function<void()> cb) { close_cb_ = std::move(cb); }

void ClientHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) { browser_ = browser; }

bool ClientHandler::DoClose(CefRefPtr<CefBrowser> /*browser*/) {
    return false; // proceed with the default close
}

void ClientHandler::OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) {
    browser_ = nullptr;
    if (close_cb_) close_cb_();
}

void ClientHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                              int /*httpStatusCode*/) {
    if (! frame || ! frame->IsMain()) return;
    bool expected = false;
    if (! property_injected_.compare_exchange_strong(expected, true)) return;
    InjectUserProperties(browser, user_props_json_, has_user_props_);
}

bool ClientHandler::OnConsoleMessage(CefRefPtr<CefBrowser> /*browser*/, cef_log_severity_t level,
                                     const CefString& message, const CefString& source, int line) {
    const char* level_str = "info";
    switch (level) {
    case LOGSEVERITY_DEBUG:   level_str = "debug"; break;
    case LOGSEVERITY_INFO:    level_str = "info"; break;
    case LOGSEVERITY_WARNING: level_str = "warn"; break;
    case LOGSEVERITY_ERROR:   level_str = "error"; break;
    case LOGSEVERITY_FATAL:   level_str = "fatal"; break;
    default: break;
    }
    std::fprintf(stderr,
                 "web [%s] %s (%s:%d)\n",
                 level_str,
                 message.ToString().c_str(),
                 source.ToString().c_str(),
                 line);
    return false; // also let CEF's default handler log
}
} // namespace wallpaper
