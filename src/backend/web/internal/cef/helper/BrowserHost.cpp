#include "backend/web/internal/cef/helper/BrowserHost.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

#include "backend/web/internal/cef/PropertyBridge.hpp"

namespace wallpaper
{
namespace
{
struct CefRuntimeState {
    std::mutex  mutex;
    std::size_t ref_count { 0 };
    bool        initialized { false };
};

CefRuntimeState& runtimeState() {
    static CefRuntimeState state;
    return state;
}

bool acquireCefRuntime(CefRefPtr<AppHandler> app, const WebBrowserHost::InitOptions& opts) {
    auto&                       state = runtimeState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.initialized) {
        ++state.ref_count;
        return true;
    }

    int         argc   = 1;
    char        arg0[] = "we-browser-host";
    char*       argv[] = { arg0 };
    CefMainArgs main_args(argc, argv);

    CefSettings settings;
    settings.no_sandbox                   = true;
    settings.windowless_rendering_enabled = true;
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

    app->SetMuteAudio(! opts.enable_audio);
    app->SetRuntimeProfile(opts.runtime_profile);
    app->SetPreferredWindowSystem(opts.preferred_window_system);
    app->SetExtraCommandLineSwitches(opts.extra_command_line_switches);

    if (! CefInitialize(main_args, settings, app.get(), nullptr)) {
        std::fprintf(stderr, "web: CefInitialize failed\n");
        return false;
    }

    state.initialized = true;
    state.ref_count   = 1;
    return true;
}

void releaseCefRuntime() {
    auto&                       state = runtimeState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.ref_count == 0) return;
    --state.ref_count;
    // CEF is process-global. Wallpaper sessions may come and go, but cycling
    // CefInitialize/CefShutdown inside one daemon process is unsupported.
}
} // namespace

CefWebBrowserHost::~CefWebBrowserHost() { Shutdown(); }

bool CefWebBrowserHost::Init(const InitOptions& opts) {
    if (! impl_) {
        impl_      = std::make_shared<Impl>();
        impl_->app = new AppHandler();
    }
    if (impl_->initialised) {
        std::fprintf(stderr, "web: WebBrowserHost::Init called twice\n");
        return false;
    }

    if (! acquireCefRuntime(impl_->app, opts)) {
        return false;
    }
    impl_->runtime_acquired         = true;
    impl_->initialised              = true;
    impl_->prefer_accelerated_paint = opts.prefer_accelerated_paint;
    impl_->should_exit.store(false);
    impl_->close_requested.store(false);
    return true;
}

bool CefWebBrowserHost::OpenWallpaper(const WebManifestData&       manifest,
                                      const std::filesystem::path& workshop_dir, int width,
                                      int height) {
    if (! impl_) return false;
    if (! impl_->initialised) {
        std::fprintf(stderr, "web: OpenWallpaper before Init\n");
        return false;
    }

    impl_->client = nullptr;
    impl_->osr    = nullptr;
    impl_->should_exit.store(false);
    impl_->close_requested.store(false);
    impl_->osr = new OsrRenderHandler();
    impl_->osr->SetViewSize(width, height);
    if (accelerated_paint_callback_) {
        impl_->osr->SetAcceleratedPaintCallback(accelerated_paint_callback_);
    }
    if (software_paint_callback_) {
        impl_->osr->SetSoftwarePaintCallback(software_paint_callback_);
    }

    impl_->client =
        new ClientHandler(manifest.user_props_json, manifest.has_user_props, impl_->osr);
    impl_->client->SetCloseCallback([this] {
        impl_->should_exit.store(true);
    });

    auto        entry = workshop_dir / manifest.entry_html;
    std::string url   = "file://" + entry.string();

    CefWindowInfo info;
    info.SetAsWindowless(0); // no parent window — pure OSR
    info.shared_texture_enabled = accelerated_paint_callback_ ? 1 : 0;

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 60;

    auto browser = CefBrowserHost::CreateBrowserSync(
        info, impl_->client.get(), url, browser_settings, nullptr, nullptr);
    if (! browser) {
        impl_->client = nullptr;
        impl_->osr    = nullptr;
        return false;
    }
    return true;
}

void CefWebBrowserHost::Invalidate() {
    if (! impl_) return;
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->Invalidate(PET_VIEW);
}

bool CefWebBrowserHost::ReopenWallpaper(const WebManifestData&       manifest,
                                        const std::filesystem::path& workshop_dir, int width,
                                        int height) {
    if (! impl_ || ! impl_->initialised) return false;
    RequestClose();
    for (int i = 0; i < 200 && ! impl_->should_exit.load(); ++i) {
        CefDoMessageLoopWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (! impl_->should_exit.load()) return false;
    return OpenWallpaper(manifest, workshop_dir, width, height);
}

void CefWebBrowserHost::OnResize(int width, int height) {
    if (! impl_) return;
    if (width <= 0 || height <= 0) return;
    if (! impl_->osr) return;
    impl_->osr->SetViewSize(width, height);
    if (! impl_->client) return;
    if (auto b = impl_->client->GetBrowser(); b && b->GetHost()) {
        b->GetHost()->WasResized();
    }
}

void CefWebBrowserHost::OnMouseMove(int x, int y, bool left_down) {
    if (! impl_) return;
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x         = x;
    ev.y         = y;
    ev.modifiers = left_down ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0;
    b->GetHost()->SendMouseMoveEvent(ev, /*mouseLeave=*/false);
}

void CefWebBrowserHost::OnMouseButton(int x, int y, int cef_button, bool down, int click_count) {
    if (! impl_) return;
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

void CefWebBrowserHost::OnMouseWheel(int x, int y, int delta_x, int delta_y) {
    if (! impl_) return;
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    b->GetHost()->SendMouseWheelEvent(ev, delta_x, delta_y);
}

void CefWebBrowserHost::OnKey(int cef_key_event_type, int native_key_code, int windows_key_code,
                              int modifiers, unsigned int unicode_char) {
    if (! impl_) return;
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

void CefWebBrowserHost::OnFocus(bool gained) {
    if (! impl_) return;
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    b->GetHost()->SetFocus(gained);
}

void CefWebBrowserHost::Pump() {
    if (! impl_) return;
    if (impl_->initialised) CefDoMessageLoopWork();
}

void CefWebBrowserHost::ApplyVolume(float volume) {
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

void CefWebBrowserHost::SetFrameRate(int fps) {
    if (! impl_) return;
    if (fps <= 0 || ! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->SetWindowlessFrameRate(fps);
}

void CefWebBrowserHost::SetPaused(bool paused) {
    if (! impl_) return;
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->WasHidden(paused);
}

void CefWebBrowserHost::ApplyUserProperty(std::string_view key, std::string_view value_json) {
    if (! impl_) return;
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b) return;
    auto frame = b->GetMainFrame();
    if (! frame) return;

    auto snippet = BuildApplyUserPropertySnippet(std::string(key), std::string(value_json));
    frame->ExecuteJavaScript(snippet, "wallpaper://internal/apply_user_property.js", 0);
}

void CefWebBrowserHost::PushAudioData(const float* data, std::size_t count) {
    if (! impl_) return;
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

bool CefWebBrowserHost::ShouldExit() const { return impl_ ? impl_->should_exit.load() : false; }

void CefWebBrowserHost::RequestClose() {
    if (! impl_) return;
    if (impl_->close_requested.exchange(true)) return;
    if (! impl_->client) {
        impl_->should_exit.store(true);
        return;
    }
    auto browser = impl_->client->GetBrowser();
    if (! browser || ! browser->GetHost()) {
        impl_->should_exit.store(true);
        return;
    }
    browser->GetHost()->CloseBrowser(true);
}

void CefWebBrowserHost::Shutdown() {
    if (! impl_) return;
    if (! impl_->initialised) return;

    RequestClose();

    for (int i = 0; i < 200 && ! impl_->should_exit.load(); ++i) {
        CefDoMessageLoopWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    impl_->client               = nullptr;
    impl_->osr                  = nullptr;
    accelerated_paint_callback_ = {};
    software_paint_callback_    = {};
    impl_->should_exit.store(true);
    impl_->close_requested.store(false);
    impl_->initialised = false;
    if (impl_->runtime_acquired) {
        releaseCefRuntime();
        impl_->runtime_acquired = false;
    }
}
} // namespace wallpaper
