#pragma once

#include "wallpaper/web/WebTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace wallpaper
{
// ---------------------------------------------------------------------------
// Wallpaper Engine *web* browser host, CEF-free public interface.
// ---------------------------------------------------------------------------
// The implementation in src/backend/web/internal/cef/BrowserHost.cpp
// owns the CefApp / CefClient / CefBrowser / CefBrowserProcessHandler
// lifecycles. The pImpl keeps CEF's libcef_dll headers off the public
// include surface — the architecture-guard test requires that any
// consumer of include/wallpaper/web/** can compile against this
// header without a CEF binary distribution installed.
//
// Lifecycle (matches the upstream WebViewer pattern):
//
//   int helper = host.RunOrExitIfHelper(argc, argv);
//   if (helper >= 0) return helper;             // CEF subprocess: exit
//   host.SetAcceleratedPaintCallback(cb);        // before Init
//   host.Init(opts);
//   host.OpenWallpaper(manifest, workshop_dir, w, h);
//   while (! host.ShouldExit()) { host.Pump(); host.Invalidate(); }
//   host.Shutdown();
class WebBrowserHost {
public:
    struct InitOptions {
        std::filesystem::path resources_dir; // CEF Resources/
        std::filesystem::path locales_dir;   // CEF Resources/locales/
        std::filesystem::path cache_dir;     // optional CEF disk cache
        bool                  enable_remote_debugging { false };
        int                   remote_debugging_port { 0 };
        // false ⇒ pass --mute-audio to Chromium so no output device opens.
        bool enable_audio { true };
    };

    WebBrowserHost();
    ~WebBrowserHost();

    WebBrowserHost(const WebBrowserHost&)            = delete;
    WebBrowserHost& operator=(const WebBrowserHost&) = delete;

    // Returns >= 0 if this process is a CEF helper (renderer / utility /
    // zygote) — caller MUST `return` that as the process exit code
    // without doing anything else. Returns -1 if this is the main
    // browser process and initialisation should continue.
    int RunOrExitIfHelper(int argc, char** argv);

    // Initialise CEF in the main browser process. Must be called exactly
    // once after RunOrExitIfHelper returns -1.
    bool Init(const InitOptions& opts);

    // Install an accelerated-paint sink. When set BEFORE OpenWallpaper
    // and the host's shared-texture path is honoured by CEF, the host
    // delivers DMA-BUF frames here instead of CPU OnPaint bitmaps.
    // Plane FDs are valid only inside the synchronous call.
    void SetAcceleratedPaintCallback(AcceleratedPaintCallback cb);

    // Spawn a windowless (OSR) browser for the wallpaper. The entry
    // HTML is loaded from `workshop_dir / manifest.entry_html`; the
    // manifest's user_props_json is injected on first load. Initial
    // logical size is `width` x `height`; resize via OnResize.
    bool OpenWallpaper(const WebManifestData&           manifest,
                       const std::filesystem::path&    workshop_dir,
                       int                              width,
                       int                              height);

    // Notify CEF that the host window changed size. Updates GetViewRect
    // so the next OnPaint matches `width` x `height`.
    void OnResize(int width, int height);

    // Force CEF to repaint the view. CEF's internal pacing in OSR
    // mode can stop emitting OnAcceleratedPaint when nothing on the
    // page appears to require redraw — this kicks it.
    void Invalidate();

    // Mouse / wheel / key / focus forwarding. Coordinates are in logical
    // pixels matching the GetViewRect rect.
    void OnMouseMove(int x, int y, bool left_down);
    void OnMouseButton(int x, int y, int cef_button, bool down, int click_count);
    void OnMouseWheel(int x, int y, int delta_x, int delta_y);
    void OnKey(int cef_key_event_type, int native_key_code, int windows_key_code, int modifiers,
               unsigned int unicode_char);
    void OnFocus(bool gained);

    // Pump the CEF message loop once. Caller drives this from their main
    // event loop alongside whatever windowing-system polling they do.
    void Pump();

    // Hot-reload setting hooks. Safe to call from the same thread that
    // drives Pump (typically the main thread).
    void ApplyVolume(float volume);                // → {audio:{value:v}}
    void SetFrameRate(int fps);                    // CefBrowserHost::SetWindowlessFrameRate
    void SetPaused(bool paused);                   // CefBrowserHost::WasHidden
    void ApplyUserProperty(std::string_view key, std::string_view value_json);

    // Feed one audio-response frame to the page. `data` is the WE web
    // layout (128 floats: 64 left + 64 right, ~0..1). Dispatched to
    // every wallpaperRegisterAudioListener callback via
    // __weweb_pushAudio. No-op until the page's V8 context exists.
    void PushAudioData(const float* data, std::size_t count);

    // True once the browser has been closed (close button, JS-driven
    // close, etc.).
    bool ShouldExit() const;

    // Flag the host for graceful exit.
    void RequestClose();

    // Tear down CEF. Safe to call multiple times.
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace wallpaper
