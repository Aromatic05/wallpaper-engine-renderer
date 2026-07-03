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
class CefWebBrowserHost final : public WebBrowserHost {
public:
    CefWebBrowserHost() = default;
    ~CefWebBrowserHost() override;

    bool Init(const InitOptions& opts) override;
    bool OpenWallpaper(const WebManifestData&        manifest,
                       const std::filesystem::path& workshop_dir,
                       int                           width,
                       int                           height) override;
    void OnResize(int width, int height) override;
    void Invalidate() override;
    void OnMouseMove(int x, int y, bool left_down) override;
    void OnMouseButton(int x, int y, int cef_button, bool down, int click_count) override;
    void OnMouseWheel(int x, int y, int delta_x, int delta_y) override;
    void OnKey(int cef_key_event_type, int native_key_code, int windows_key_code,
               int modifiers, unsigned int unicode_char) override;
    void OnFocus(bool gained) override;
    void Pump() override;
    void ApplyVolume(float volume) override;
    void SetFrameRate(int fps) override;
    void SetPaused(bool paused) override;
    void ApplyUserProperty(std::string_view key, std::string_view value_json) override;
    void PushAudioData(const float* data, std::size_t count) override;
    bool ShouldExit() const override;
    void RequestClose() override;
    void Shutdown() override;

private:
    struct Impl {
        CefRefPtr<AppHandler>       app;
        CefRefPtr<OsrRenderHandler> osr;
        CefRefPtr<ClientHandler>    client;
        std::atomic<bool>           should_exit { false };
        std::atomic<bool>           close_requested { false };
        bool                        initialised { false };
        bool                        runtime_acquired { false };
        bool                        prefer_accelerated_paint { true };
    };

    std::shared_ptr<Impl> impl_;
};
} // namespace wallpaper
