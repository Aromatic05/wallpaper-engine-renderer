#include "wallpaper/web/WebBrowserHost.hpp"

#include "backend/web/internal/cef/CefHostApi.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wallpaper
{
namespace
{
void dlopenSelfAnchor() {}

constexpr int kDlOpenFlags = RTLD_NOW | RTLD_LOCAL
#ifdef RTLD_NODELETE
                             | RTLD_NODELETE
#endif
    ;

class DlCefWebBrowserHost final : public WebBrowserHost {
public:
    ~DlCefWebBrowserHost() override {
        Shutdown();
        if (api_ && host_) {
            api_->destroy_host(host_);
            host_ = nullptr;
        }
    }

    bool Init(const InitOptions& opts) override {
        if (host_) {
            std::fprintf(stderr, "web: WebBrowserHost::Init called twice\n");
            return false;
        }

        const std::string resources_dir          = opts.resources_dir.string();
        const std::string locales_dir            = opts.locales_dir.string();
        const std::string cache_dir              = opts.cache_dir.string();
        const std::string browser_subprocess_dir = opts.browser_subprocess_path.string();
        configured_helper_path_                  = browser_subprocess_dir;

        if (! ensureLoaded()) {
            reportApiError("failed to load we-cef-helper");
            return false;
        }

        host_ = api_->create_host();
        if (! host_) {
            reportApiError("failed to create CEF host");
            return false;
        }

        syncCallbacks();

        std::vector<const char*> extra_switches;
        extra_switches.reserve(opts.extra_command_line_switches.size());
        for (const auto& entry : opts.extra_command_line_switches) {
            extra_switches.push_back(entry.c_str());
        }

        WeCefInitOptions init_opts {};
        init_opts.struct_size                 = static_cast<uint32_t>(sizeof(init_opts));
        init_opts.resources_dir               = resources_dir.c_str();
        init_opts.locales_dir                 = locales_dir.c_str();
        init_opts.cache_dir                   = cache_dir.c_str();
        init_opts.browser_subprocess_path     = browser_subprocess_dir.c_str();
        init_opts.enable_remote_debugging     = opts.enable_remote_debugging ? 1 : 0;
        init_opts.remote_debugging_port       = opts.remote_debugging_port;
        init_opts.enable_audio                = opts.enable_audio ? 1 : 0;
        init_opts.runtime_profile             = static_cast<int>(opts.runtime_profile);
        init_opts.preferred_window_system     = static_cast<int>(opts.preferred_window_system);
        init_opts.prefer_accelerated_paint    = opts.prefer_accelerated_paint ? 1 : 0;
        init_opts.extra_command_line_switches = {
            extra_switches.data(),
            extra_switches.size(),
        };

        if (api_->init(host_, &init_opts) == 0) {
            reportApiError("CEF host init failed");
            api_->destroy_host(host_);
            host_ = nullptr;
            return false;
        }
        return true;
    }

    void SetAcceleratedPaintCallback(AcceleratedPaintCallback cb) override {
        WebBrowserHost::SetAcceleratedPaintCallback(std::move(cb));
        syncCallbacks();
    }

    void SetSoftwarePaintCallback(SoftwarePaintCallback cb) override {
        WebBrowserHost::SetSoftwarePaintCallback(std::move(cb));
        syncCallbacks();
    }

    bool OpenWallpaper(const WebManifestData& manifest, const std::filesystem::path& workshop_dir,
                       int width, int height) override {
        if (! host_ || ! api_) {
            std::fprintf(stderr, "web: OpenWallpaper before Init\n");
            return false;
        }

        const std::string title           = manifest.title;
        const std::string entry_html      = manifest.entry_html;
        const std::string user_props_json = manifest.user_props_json;
        const std::string workshop_path   = workshop_dir.string();

        WeCefManifestData manifest_data {};
        manifest_data.struct_size     = static_cast<uint32_t>(sizeof(manifest_data));
        manifest_data.title           = title.c_str();
        manifest_data.entry_html      = entry_html.c_str();
        manifest_data.user_props_json = user_props_json.c_str();
        manifest_data.has_user_props  = manifest.has_user_props ? 1 : 0;

        if (api_->open_wallpaper(host_, &manifest_data, workshop_path.c_str(), width, height) ==
            0) {
            reportApiError("CEF open wallpaper failed");
            return false;
        }
        return true;
    }

    bool ReopenWallpaper(const WebManifestData& manifest, const std::filesystem::path& workshop_dir,
                         int width, int height) override {
        if (! host_ || ! api_) return false;
        RequestClose();
        for (int i = 0; i < 200 && ! ShouldExit(); ++i) {
            Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (! ShouldExit()) return false;
        return OpenWallpaper(manifest, workshop_dir, width, height);
    }

    void OnResize(int width, int height) override {
        if (host_ && api_) api_->on_resize(host_, width, height);
    }

    void Invalidate() override {
        if (host_ && api_) api_->invalidate(host_);
    }

    void OnMouseMove(int x, int y, bool left_down) override {
        if (host_ && api_) api_->on_mouse_move(host_, x, y, left_down ? 1 : 0);
    }

    void OnMouseButton(int x, int y, int cef_button, bool down, int click_count) override {
        if (host_ && api_) {
            api_->on_mouse_button(host_, x, y, cef_button, down ? 1 : 0, click_count);
        }
    }

    void OnMouseWheel(int x, int y, int delta_x, int delta_y) override {
        if (host_ && api_) api_->on_mouse_wheel(host_, x, y, delta_x, delta_y);
    }

    void OnKey(int cef_key_event_type, int native_key_code, int windows_key_code, int modifiers,
               unsigned int unicode_char) override {
        if (host_ && api_) {
            api_->on_key(host_,
                         cef_key_event_type,
                         native_key_code,
                         windows_key_code,
                         modifiers,
                         unicode_char);
        }
    }

    void OnFocus(bool gained) override {
        if (host_ && api_) api_->on_focus(host_, gained ? 1 : 0);
    }

    void Pump() override {
        if (host_ && api_) api_->pump(host_);
    }

    void ApplyVolume(float volume) override {
        if (host_ && api_) api_->apply_volume(host_, volume);
    }

    void SetFrameRate(int fps) override {
        if (host_ && api_) api_->set_frame_rate(host_, fps);
    }

    void SetPaused(bool paused) override {
        if (host_ && api_) api_->set_paused(host_, paused ? 1 : 0);
    }

    void ApplyUserProperty(std::string_view key, std::string_view value_json) override {
        if (! host_ || ! api_) return;
        const WeCefStringView key_view { key.data(), key.size() };
        const WeCefStringView value_view { value_json.data(), value_json.size() };
        api_->apply_user_property(host_, key_view, value_view);
    }

    void PushAudioData(const float* data, std::size_t count) override {
        if (host_ && api_) api_->push_audio_data(host_, data, count);
    }

    bool ShouldExit() const override {
        return host_ && api_ ? api_->should_exit(host_) != 0 : false;
    }

    void RequestClose() override {
        if (host_ && api_) api_->request_close(host_);
    }

    void Shutdown() override {
        if (! host_ || ! api_) return;
        api_->shutdown(host_);
        api_->destroy_host(host_);
        host_ = nullptr;
    }

private:
    static void onAcceleratedPaint(void* userdata, const WeCefDmaBufFrame* frame) {
        if (! userdata || ! frame) return;
        auto* self = static_cast<DlCefWebBrowserHost*>(userdata);
        if (! self->accelerated_paint_callback_) return;

        DmaBufFrame out;
        out.plane_count = frame->plane_count;
        if (out.plane_count > 4) out.plane_count = 4;
        for (int i = 0; i < out.plane_count; ++i) {
            out.planes[i].fd     = frame->planes[i].fd;
            out.planes[i].stride = frame->planes[i].stride;
            out.planes[i].offset = frame->planes[i].offset;
            out.planes[i].size   = frame->planes[i].size;
        }
        out.modifier       = frame->modifier;
        out.format         = static_cast<DmaBufFormat>(frame->format);
        out.coded_width    = frame->coded_width;
        out.coded_height   = frame->coded_height;
        out.visible_x      = frame->visible_x;
        out.visible_y      = frame->visible_y;
        out.visible_width  = frame->visible_width;
        out.visible_height = frame->visible_height;
        self->accelerated_paint_callback_(out);
    }

    static void onSoftwarePaint(void* userdata, const WeCefSoftwareFrame* frame) {
        if (! userdata || ! frame) return;
        auto* self = static_cast<DlCefWebBrowserHost*>(userdata);
        if (! self->software_paint_callback_) return;
        self->software_paint_callback_(
            frame->buffer, frame->width, frame->height, frame->stride_bytes);
    }

    static std::filesystem::path selfDirectory() {
        Dl_info info {};
        if (dladdr(reinterpret_cast<void*>(&dlopenSelfAnchor), &info) == 0) {
            return {};
        }
        if (! info.dli_fname) return {};

        std::error_code ec;
        const auto      full_path = std::filesystem::weakly_canonical(info.dli_fname, ec);
        if (ec) {
            return std::filesystem::path(info.dli_fname).parent_path();
        }
        return full_path.parent_path();
    }

    bool ensureLoaded() {
        if (dl_handle_ && api_) return true;

        std::vector<std::string> candidates;
        if (const char* env_path = std::getenv("WE_CEF_HELPER_PATH")) {
            if (env_path[0] != '\0') candidates.emplace_back(env_path);
        }
        if (! configured_helper_path_.empty()) {
            candidates.push_back(configured_helper_path_);
        }

        const auto dir = selfDirectory();
        if (! dir.empty()) {
            candidates.push_back((dir / "we-cef-helper").string());
            candidates.push_back((dir / "backend" / "web" / "we-cef-helper").string());
        }
        candidates.emplace_back("we-cef-helper");

        for (const auto& candidate : candidates) {
            void* handle = dlopen(candidate.c_str(), kDlOpenFlags);
            if (! handle) {
                last_load_error_ = dlerrorString();
                continue;
            }

            auto* get_api =
                reinterpret_cast<WeCefGetHostApiFn>(dlsym(handle, "we_cef_get_host_api"));
            if (! get_api) {
                last_load_error_ = dlerrorString();
                dlclose(handle);
                continue;
            }

            const WeCefHostApi* api = get_api(WE_CEF_HOST_API_VERSION);
            if (! validateApi(api)) {
                dlclose(handle);
                return false;
            }

            dl_handle_ = handle;
            api_       = api;
            return true;
        }

        return false;
    }

    bool validateApi(const WeCefHostApi* api) {
        if (! api) {
            reportApiError("we_cef_get_host_api returned null");
            return false;
        }
        if (api->version != WE_CEF_HOST_API_VERSION) {
            std::fprintf(stderr,
                         "web: unsupported CEF host api version %u\n",
                         static_cast<unsigned>(api->version));
            return false;
        }
        if (api->struct_size < sizeof(WeCefHostApi)) {
            std::fprintf(stderr, "web: CEF host api struct too small\n");
            return false;
        }
        if (! api->create_host || ! api->destroy_host || ! api->set_callbacks || ! api->init ||
            ! api->open_wallpaper || ! api->on_resize || ! api->invalidate ||
            ! api->on_mouse_move || ! api->on_mouse_button || ! api->on_mouse_wheel ||
            ! api->on_key || ! api->on_focus || ! api->pump || ! api->apply_volume ||
            ! api->set_frame_rate || ! api->set_paused || ! api->apply_user_property ||
            ! api->push_audio_data || ! api->should_exit || ! api->request_close ||
            ! api->shutdown || ! api->helper_main || ! api->last_error) {
            std::fprintf(stderr, "web: CEF host api is missing required functions\n");
            return false;
        }
        api_ = api;
        return true;
    }

    void syncCallbacks() {
        if (! host_ || ! api_) return;
        WeCefCallbacks callbacks {};
        callbacks.struct_size = static_cast<uint32_t>(sizeof(callbacks));
        callbacks.userdata    = this;
        callbacks.on_accelerated_paint =
            accelerated_paint_callback_ ? &DlCefWebBrowserHost::onAcceleratedPaint : nullptr;
        callbacks.on_software_paint =
            software_paint_callback_ ? &DlCefWebBrowserHost::onSoftwarePaint : nullptr;
        api_->set_callbacks(host_, &callbacks);
    }

    void reportApiError(const char* context) const {
        const char* detail = nullptr;
        if (api_ && api_->last_error) {
            detail = api_->last_error();
        }
        if (! detail || detail[0] == '\0') {
            detail = last_load_error_.empty() ? "unknown error" : last_load_error_.c_str();
        }
        std::fprintf(stderr, "web: %s: %s\n", context, detail);
    }

    static std::string dlerrorString() {
        const char* value = dlerror();
        return value ? std::string(value) : std::string {};
    }

    void*               dl_handle_ { nullptr };
    const WeCefHostApi* api_ { nullptr };
    WeCefHostHandle*    host_ { nullptr };
    std::string         last_load_error_;
    std::string         configured_helper_path_;
};
} // namespace

std::shared_ptr<WebBrowserHost> CreateCefWebBrowserHost() {
    return std::make_shared<DlCefWebBrowserHost>();
}
} // namespace wallpaper
