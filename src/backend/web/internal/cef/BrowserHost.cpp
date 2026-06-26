#include "backend/web/internal/cef/BrowserHost.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "backend/web/internal/cef/UserProperties.hpp"

namespace wallpaper
{
WebBrowserHost::WebBrowserHost(): impl_(std::make_unique<Impl>()) { impl_->app = new AppHandler(); }

WebBrowserHost::~WebBrowserHost() { Shutdown(); }

bool WebBrowserHost::Init(const InitOptions& opts) {
    if (impl_->initialised) {
        std::fprintf(stderr, "web: WebBrowserHost::Init called twice\n");
        return false;
    }

    int argc = 1;
    char arg0[] = "we-browser-host";
    char* argv[] = { arg0 };
    CefMainArgs main_args(argc, argv);

    CefSettings settings;
    settings.no_sandbox                   = true;
    settings.windowless_rendering_enabled = true; // OSR mode
    settings.multi_threaded_message_loop  = false;
    settings.log_severity                 = LOGSEVERITY_WARNING;

    auto set_cef_path = [](cef_string_t* dest, const std::filesystem::path& p) {
        if (p.empty()) return;
        CefString cef_str { dest };
        cef_str = p.string();
    };
    set_cef_path(&settings.resources_dir_path, opts.resources_dir);
    set_cef_path(&settings.locales_dir_path, opts.locales_dir);
    set_cef_path(&settings.root_cache_path, opts.cache_dir);
    set_cef_path(&settings.browser_subprocess_path, opts.browser_subprocess_path);

    if (opts.enable_remote_debugging && opts.remote_debugging_port > 0) {
        settings.remote_debugging_port = opts.remote_debugging_port;
    }

    // Stash before CefInitialize; AppHandler::OnBeforeCommandLineProcessing
    // runs synchronously inside it and reads the flag.
    impl_->app->SetMuteAudio(! opts.enable_audio);

    if (! CefInitialize(main_args, settings, impl_->app.get(), nullptr)) {
        std::fprintf(stderr, "web: CefInitialize failed\n");
        return false;
    }
    impl_->initialised = true;
    return true;
}

bool WebBrowserHost::OpenWallpaper(const WebManifestData&           manifest,
                                   const std::filesystem::path&    workshop_dir,
                                   int                              width,
                                   int                              height) {
    if (! impl_->initialised) {
        std::fprintf(stderr, "web: OpenWallpaper before Init\n");
        return false;
    }

    impl_->osr = new OsrRenderHandler();
    impl_->osr->SetViewSize(width, height);
    if (impl_->accel_cb) {
        impl_->osr->SetAcceleratedPaintCallback(impl_->accel_cb);
    }

    impl_->client = new ClientHandler(manifest.user_props_json, manifest.has_user_props, impl_->osr);
    impl_->client->SetCloseCallback([this] {
        impl_->should_exit.store(true);
    });

    auto        entry = workshop_dir / manifest.entry_html;
    std::string url   = "file://" + entry.string();

    CefWindowInfo info;
    info.SetAsWindowless(0);         // no parent window — pure OSR
    info.shared_texture_enabled = 1; // request DMA-BUF / OnAcceleratedPaint

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 60;

    CefBrowserHost::CreateBrowser(
        info, impl_->client.get(), url, browser_settings, nullptr, nullptr);
    return true;
}

void WebBrowserHost::SetAcceleratedPaintCallback(AcceleratedPaintCallback cb) {
    impl_->accel_cb = std::move(cb);
    if (impl_->osr) impl_->osr->SetAcceleratedPaintCallback(impl_->accel_cb);
}

void WebBrowserHost::Invalidate() {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->Invalidate(PET_VIEW);
}

void WebBrowserHost::OnResize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (! impl_->osr) return;
    impl_->osr->SetViewSize(width, height);
    if (! impl_->client) return;
    if (auto b = impl_->client->GetBrowser(); b && b->GetHost()) {
        b->GetHost()->WasResized();
    }
}

void WebBrowserHost::OnMouseMove(int x, int y, bool left_down) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x         = x;
    ev.y         = y;
    ev.modifiers = left_down ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0;
    b->GetHost()->SendMouseMoveEvent(ev, /*mouseLeave=*/false);
}

void WebBrowserHost::OnMouseButton(int x, int y, int cef_button, bool down, int click_count) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    b->GetHost()->SendMouseClickEvent(ev,
                                      static_cast<cef_mouse_button_type_t>(cef_button),
                                      /*mouseUp=*/! down,
                                      click_count);
}

void WebBrowserHost::OnMouseWheel(int x, int y, int delta_x, int delta_y) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    b->GetHost()->SendMouseWheelEvent(ev, delta_x, delta_y);
}

void WebBrowserHost::OnKey(int cef_key_event_type, int native_key_code, int windows_key_code,
                           int modifiers, unsigned int unicode_char) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefKeyEvent ev;
    ev.type                 = static_cast<cef_key_event_type_t>(cef_key_event_type);
    ev.native_key_code      = native_key_code;
    ev.windows_key_code     = windows_key_code;
    ev.modifiers            = static_cast<uint32_t>(modifiers);
    ev.character            = static_cast<char16_t>(unicode_char);
    ev.unmodified_character = ev.character;
    b->GetHost()->SendKeyEvent(ev);
}

void WebBrowserHost::OnFocus(bool gained) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    b->GetHost()->SetFocus(gained);
}

void WebBrowserHost::Pump() {
    if (impl_->initialised) CefDoMessageLoopWork();
}

void WebBrowserHost::ApplyVolume(float volume) {
    // Wallpaper Engine's web convention: pack into the standard
    // applyUserProperties envelope under the `audio` key. The page's
    // wallpaperPropertyListener is expected to map this onto the
    // in-page gain.
    std::string value_json;
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "{\"value\":%.4f}", static_cast<double>(volume));
        value_json.assign(buf);
    }
    ApplyUserProperty("audio", value_json);
}

void WebBrowserHost::SetFrameRate(int fps) {
    if (fps <= 0 || ! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->SetWindowlessFrameRate(fps);
}

void WebBrowserHost::SetPaused(bool paused) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->WasHidden(paused);
}

void WebBrowserHost::ApplyUserProperty(std::string_view key, std::string_view value_json) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b) return;
    auto frame = b->GetMainFrame();
    if (! frame) return;

    auto snippet = BuildApplyUserPropertySnippet(std::string(key), std::string(value_json));
    frame->ExecuteJavaScript(snippet, "wallpaper://internal/apply_user_property.js", 0);
}

void WebBrowserHost::PushAudioData(const float* data, std::size_t count) {
    if (! impl_->client || ! data || count == 0) return;
    auto b = impl_->client->GetBrowser();
    if (! b) return;
    auto frame = b->GetMainFrame();
    if (! frame) return;

    std::string snippet;
    snippet.reserve(count * 8 + 64);
    snippet += "(function(){if(!window.__weweb_pushAudio)return;window.__weweb_pushAudio([";
    char buf[32];
    for (std::size_t i = 0; i < count; ++i) {
        if (i) snippet += ',';
        std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(data[i]));
        snippet += buf;
    }
    snippet += "]);})();";
    frame->ExecuteJavaScript(snippet, "wallpaper://internal/push_audio.js", 0);
}

bool WebBrowserHost::ShouldExit() const { return impl_->should_exit.load(); }

void WebBrowserHost::RequestClose() { impl_->should_exit.store(true); }

void WebBrowserHost::Shutdown() {
    if (! impl_->initialised) return;
    CefShutdown();
    impl_->initialised = false;
}
} // namespace wallpaper
