#pragma once

#include "include/cef_app.h"
#include "include/cef_browser_process_handler.h"
#include "include/cef_render_process_handler.h"

#include "wallpaper/web/WebEngineServices.hpp"

#include <string>
#include <vector>

namespace wallpaper
{
// CEF application-level handler. Owns the command-line tweaks Wallpaper
// Engine web wallpapers depend on (file:// allow-list, mute-audio,
// ANGLE-on-Wayland, vaapi decode flags, …) and installs the in-page
// wallpaperRegisterAudioListener + __weweb_pushAudio helpers in every
// frame's V8 context.
class AppHandler : public CefApp,
                   public CefBrowserProcessHandler,
                   public CefRenderProcessHandler {
public:
    AppHandler();

    // true ⇒ append --mute-audio so Chromium never opens an output
    // device. Must be set BEFORE Init() runs.
    void SetMuteAudio(bool m) { m_mute_audio = m; }
    void SetRuntimeProfile(WebCefRuntimeProfile profile) { m_runtime_profile = profile; }
    void SetPreferredWindowSystem(WebCefWindowSystem system) { m_window_system = system; }
    void SetExtraCommandLineSwitches(std::vector<std::string> switches) {
        m_extra_switches = std::move(switches);
    }

    // CefApp.
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler>  GetRenderProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString&          process_type,
                                       CefRefPtr<CefCommandLine> cmd) override;

    // CefBrowserProcessHandler.
    void OnContextInitialized() override;

    // CefRenderProcessHandler. Runs in the render process before any
    // page script — installs the WE web audio API.
    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override;

private:
    static void appendSwitch(CefRefPtr<CefCommandLine> cmd, const std::string& entry);

    bool m_mute_audio { false };
    WebCefRuntimeProfile m_runtime_profile { WebCefRuntimeProfile::Default };
    WebCefWindowSystem   m_window_system { WebCefWindowSystem::Auto };
    std::vector<std::string> m_extra_switches;

    IMPLEMENT_REFCOUNTING(AppHandler);
    DISALLOW_COPY_AND_ASSIGN(AppHandler);
};
} // namespace wallpaper
