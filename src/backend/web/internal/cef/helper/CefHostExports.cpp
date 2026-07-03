#include "backend/web/internal/cef/CefHostApi.h"

#include "backend/web/internal/cef/helper/AppHandler.hpp"
#include "backend/web/internal/cef/helper/BrowserHost.hpp"

#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct WeCefHostHandle {
    wallpaper::CefWebBrowserHost* impl { nullptr };
    WeCefCallbacks                callbacks {};
};

namespace wallpaper
{
void SetCefHostLastError(std::string message);

namespace
{
thread_local std::string g_last_error;

void setLastError(std::string message) { SetCefHostLastError(std::move(message)); }

bool requireStructSize(const char* name, uint32_t actual, std::size_t expected) {
    if (actual >= expected) return true;
    setLastError(std::string(name) + " has unexpected struct_size");
    return false;
}

std::string toOwnedString(WeCefStringView view) {
    if (! view.data || view.len == 0) return {};
    return std::string(view.data, view.len);
}

void configureAppFromEnvironment(CefRefPtr<AppHandler> app) {
    if (const char* value = std::getenv("WE_CEF_PROFILE")) {
        const std::string profile { value };
        if (profile == "compat" || profile == "compatibility") {
            app->SetRuntimeProfile(WebCefRuntimeProfile::Compatibility);
        } else if (profile == "debug") {
            app->SetRuntimeProfile(WebCefRuntimeProfile::Debug);
        }
    }
    if (const char* value = std::getenv("WE_CEF_WINDOW_SYSTEM")) {
        const std::string system { value };
        if (system == "x11") {
            app->SetPreferredWindowSystem(WebCefWindowSystem::X11);
        } else if (system == "wayland") {
            app->SetPreferredWindowSystem(WebCefWindowSystem::Wayland);
        }
    }
}

int helperMainImpl(int argc, char** argv) {
    CefMainArgs args(argc, argv);
    CefRefPtr<AppHandler> app = new AppHandler();
    configureAppFromEnvironment(app);
    return CefExecuteProcess(args, app.get(), nullptr);
}

void applyCallbacks(WeCefHostHandle* host);

WeCefHostHandle* createHost() {
    auto* handle = new (std::nothrow) WeCefHostHandle();
    if (! handle) {
        setLastError("failed to allocate CEF host handle");
        return nullptr;
    }

    handle->impl = new (std::nothrow) CefWebBrowserHost();
    if (! handle->impl) {
        delete handle;
        setLastError("failed to allocate CefWebBrowserHost");
        return nullptr;
    }
    return handle;
}

void destroyHost(WeCefHostHandle* host) {
    if (! host) return;
    if (host->impl) {
        host->impl->Shutdown();
        delete host->impl;
        host->impl = nullptr;
    }
    delete host;
}

void setCallbacks(WeCefHostHandle* host, const WeCefCallbacks* callbacks) {
    if (! host || ! host->impl) return;

    host->callbacks = {};
    if (callbacks) {
        if (! requireStructSize("WeCefCallbacks", callbacks->struct_size, sizeof(WeCefCallbacks))) {
            return;
        }
        host->callbacks = *callbacks;
    }
    applyCallbacks(host);
}

bool initHostImpl(WeCefHostHandle* host, const WeCefInitOptions* opts) {
    if (! host || ! host->impl || ! opts) {
        setLastError("invalid init arguments");
        return false;
    }
    if (! requireStructSize("WeCefInitOptions", opts->struct_size, sizeof(WeCefInitOptions))) {
        return false;
    }

    WebBrowserHost::InitOptions init_opts;
    if (opts->resources_dir) init_opts.resources_dir = opts->resources_dir;
    if (opts->locales_dir) init_opts.locales_dir = opts->locales_dir;
    if (opts->cache_dir) init_opts.cache_dir = opts->cache_dir;
    if (opts->browser_subprocess_path) {
        init_opts.browser_subprocess_path = opts->browser_subprocess_path;
    }
    init_opts.enable_remote_debugging  = opts->enable_remote_debugging != 0;
    init_opts.remote_debugging_port    = opts->remote_debugging_port;
    init_opts.enable_audio             = opts->enable_audio != 0;
    init_opts.runtime_profile          = static_cast<WebCefRuntimeProfile>(opts->runtime_profile);
    init_opts.preferred_window_system =
        static_cast<WebCefWindowSystem>(opts->preferred_window_system);
    init_opts.prefer_accelerated_paint = opts->prefer_accelerated_paint != 0;
    init_opts.extra_command_line_switches.reserve(opts->extra_command_line_switches.count);
    for (size_t i = 0; i < opts->extra_command_line_switches.count; ++i) {
        const char* entry = opts->extra_command_line_switches.items[i];
        init_opts.extra_command_line_switches.emplace_back(entry ? entry : "");
    }

    applyCallbacks(host);
    if (! host->impl->Init(init_opts)) {
        setLastError("CefWebBrowserHost::Init failed");
        return false;
    }
    return true;
}

bool openWallpaperImpl(WeCefHostHandle* host,
                       const WeCefManifestData* manifest,
                       const char* workshop_dir,
                       int width,
                       int height) {
    if (! host || ! host->impl || ! manifest || ! workshop_dir) {
        setLastError("invalid open_wallpaper arguments");
        return false;
    }
    if (! requireStructSize("WeCefManifestData", manifest->struct_size, sizeof(WeCefManifestData))) {
        return false;
    }

    WebManifestData manifest_data;
    manifest_data.title          = manifest->title ? manifest->title : "";
    manifest_data.entry_html     = manifest->entry_html ? manifest->entry_html : "";
    manifest_data.user_props_json = manifest->user_props_json ? manifest->user_props_json : "";
    manifest_data.has_user_props = manifest->has_user_props != 0;

    if (! host->impl->OpenWallpaper(manifest_data, workshop_dir, width, height)) {
        setLastError("CefWebBrowserHost::OpenWallpaper failed");
        return false;
    }
    return true;
}

void applyCallbacks(WeCefHostHandle* host) {
    if (! host || ! host->impl) return;

    if (host->callbacks.on_accelerated_paint) {
        host->impl->SetAcceleratedPaintCallback([host](const DmaBufFrame& frame) {
            WeCefDmaBufFrame out {};
            out.struct_size = static_cast<uint32_t>(sizeof(out));
            out.plane_count = frame.plane_count;
            if (out.plane_count > 4) out.plane_count = 4;
            for (int i = 0; i < out.plane_count; ++i) {
                out.planes[i].fd     = frame.planes[i].fd;
                out.planes[i].stride = frame.planes[i].stride;
                out.planes[i].offset = frame.planes[i].offset;
                out.planes[i].size   = frame.planes[i].size;
            }
            out.modifier       = frame.modifier;
            out.format         = static_cast<int>(frame.format);
            out.coded_width    = frame.coded_width;
            out.coded_height   = frame.coded_height;
            out.visible_x      = frame.visible_x;
            out.visible_y      = frame.visible_y;
            out.visible_width  = frame.visible_width;
            out.visible_height = frame.visible_height;
            host->callbacks.on_accelerated_paint(host->callbacks.userdata, &out);
        });
    } else {
        host->impl->SetAcceleratedPaintCallback({});
    }

    if (host->callbacks.on_software_paint) {
        host->impl->SetSoftwarePaintCallback([host](const void* buffer,
                                                    int         width,
                                                    int         height,
                                                    int         stride_bytes) {
            const WeCefSoftwareFrame frame {
                static_cast<uint32_t>(sizeof(WeCefSoftwareFrame)),
                buffer,
                width,
                height,
                stride_bytes,
            };
            host->callbacks.on_software_paint(host->callbacks.userdata, &frame);
        });
    } else {
        host->impl->SetSoftwarePaintCallback({});
    }
}

int initHost(WeCefHostHandle* host, const WeCefInitOptions* opts) {
    return initHostImpl(host, opts) ? 1 : 0;
}

int openWallpaper(WeCefHostHandle* host,
                  const WeCefManifestData* manifest,
                  const char* workshop_dir,
                  int width,
                  int height) {
    return openWallpaperImpl(host, manifest, workshop_dir, width, height) ? 1 : 0;
}

void onResize(WeCefHostHandle* host, int width, int height) {
    if (host && host->impl) host->impl->OnResize(width, height);
}

void invalidate(WeCefHostHandle* host) {
    if (host && host->impl) host->impl->Invalidate();
}

void onMouseMove(WeCefHostHandle* host, int x, int y, int left_down) {
    if (host && host->impl) host->impl->OnMouseMove(x, y, left_down != 0);
}

void onMouseButton(WeCefHostHandle* host, int x, int y, int cef_button, int down, int click_count) {
    if (host && host->impl) host->impl->OnMouseButton(x, y, cef_button, down != 0, click_count);
}

void onMouseWheel(WeCefHostHandle* host, int x, int y, int delta_x, int delta_y) {
    if (host && host->impl) host->impl->OnMouseWheel(x, y, delta_x, delta_y);
}

void onKey(WeCefHostHandle* host,
           int cef_key_event_type,
           int native_key_code,
           int windows_key_code,
           int modifiers,
           unsigned int unicode_char) {
    if (host && host->impl) {
        host->impl->OnKey(
            cef_key_event_type, native_key_code, windows_key_code, modifiers, unicode_char);
    }
}

void onFocus(WeCefHostHandle* host, int gained) {
    if (host && host->impl) host->impl->OnFocus(gained != 0);
}

void pump(WeCefHostHandle* host) {
    if (host && host->impl) host->impl->Pump();
}

void applyVolume(WeCefHostHandle* host, float volume) {
    if (host && host->impl) host->impl->ApplyVolume(volume);
}

void setFrameRate(WeCefHostHandle* host, int fps) {
    if (host && host->impl) host->impl->SetFrameRate(fps);
}

void setPaused(WeCefHostHandle* host, int paused) {
    if (host && host->impl) host->impl->SetPaused(paused != 0);
}

void applyUserProperty(WeCefHostHandle* host, WeCefStringView key, WeCefStringView value_json) {
    if (host && host->impl) {
        const std::string key_string   = toOwnedString(key);
        const std::string value_string = toOwnedString(value_json);
        host->impl->ApplyUserProperty(key_string, value_string);
    }
}

void pushAudioData(WeCefHostHandle* host, const float* data, size_t count) {
    if (host && host->impl) host->impl->PushAudioData(data, count);
}

int shouldExit(WeCefHostHandle* host) {
    return host && host->impl && host->impl->ShouldExit() ? 1 : 0;
}

void requestClose(WeCefHostHandle* host) {
    if (host && host->impl) host->impl->RequestClose();
}

void shutdown(WeCefHostHandle* host) {
    if (host && host->impl) host->impl->Shutdown();
}

int helperMain(int argc, char** argv) { return helperMainImpl(argc, argv); }

const char* lastError() { return g_last_error.c_str(); }

const WeCefHostApi kHostApi = {
    WE_CEF_HOST_API_VERSION,
    static_cast<uint32_t>(sizeof(WeCefHostApi)),
    &createHost,
    &destroyHost,
    &setCallbacks,
    &initHost,
    &openWallpaper,
    &onResize,
    &invalidate,
    &onMouseMove,
    &onMouseButton,
    &onMouseWheel,
    &onKey,
    &onFocus,
    &pump,
    &applyVolume,
    &setFrameRate,
    &setPaused,
    &applyUserProperty,
    &pushAudioData,
    &shouldExit,
    &requestClose,
    &shutdown,
    &helperMain,
    &lastError,
};
} // namespace

int RunCefHelperMain(int argc, char** argv) { return helperMainImpl(argc, argv); }

void SetCefHostLastError(std::string message) { g_last_error = std::move(message); }
} // namespace wallpaper

extern "C" WE_CEF_EXPORT const WeCefHostApi* we_cef_get_host_api(uint32_t version) {
    if (version != WE_CEF_HOST_API_VERSION) {
        wallpaper::SetCefHostLastError("unsupported WeCefHostApi version");
        return nullptr;
    }
    return &wallpaper::kHostApi;
}
