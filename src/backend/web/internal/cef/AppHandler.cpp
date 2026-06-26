#include "backend/web/internal/cef/AppHandler.hpp"

#include "include/cef_command_line.h"

#include "backend/web/internal/cef/UserProperties.hpp"

namespace wallpaper
{
AppHandler::AppHandler() = default;

void AppHandler::OnBeforeCommandLineProcessing(const CefString&          process_type,
                                               CefRefPtr<CefCommandLine> cmd) {
    // Only tweak the browser process command line. Renderer / utility
    // helpers inherit the relevant switches from the browser anyway.
    if (! process_type.empty()) return;

    // WE web wallpapers are loose directory trees loaded as file:// URLs.
    cmd->AppendSwitch("allow-file-access-from-files");
    cmd->AppendSwitch("disable-web-security");
    cmd->AppendSwitch("allow-chrome-scheme-url");

    // The `minimal` CEF Linux distribution does not ship the SUID sandbox
    // helper. Match `CefSettings.no_sandbox = true` at the cmdline level
    // so the switch propagates to all child procs.
    cmd->AppendSwitch("no-sandbox");

    cmd->AppendSwitch("off-screen-rendering-enabled");
    cmd->AppendSwitch("shared-texture-enabled");

    std::string features { "AcceleratedVideoDecodeLinuxZeroCopyGL,"
                           "AcceleratedVideoDecodeLinuxGL,"
                           "VaapiIgnoreDriverChecks,"
                           "VaapiOnNvidiaGPUs,"
                           "VaapiVideoDecodeLinuxGL" };
    std::string dis_features;
    if (cmd->HasSwitch("disable-features")) {
        dis_features = cmd->GetSwitchValue("disable-features").ToString();
        if (! dis_features.empty()) dis_features += ",";
        dis_features +=
            "Crashpad,AutofillServerCommunication,HardwareMediaKeyHandling,WebBluetooth,WebUSB";
    }

    // EGL-on-ANGLE-on-Wayland; we deliberately do NOT enable Vulkan from
    // the CEF side — the page's frames come through OnAcceleratedPaint
    // and are imported into our own Vulkan device in the web backend.
    cmd->AppendSwitchWithValue("use-gl", "angle");
    cmd->AppendSwitchWithValue("use-angle", "gl-egl");
    cmd->AppendSwitchWithValue("use-vulkan", "disabled");
    cmd->AppendSwitch("disable-vulkan-surface");
    dis_features += ",Vulkan,VulkanFromANGLE,DefaultAngleVulkan,SkiaGraphite";

    cmd->AppendSwitchWithValue("ozone-platform", "wayland");
    cmd->AppendSwitch("enable-gpu");
    cmd->AppendSwitch("ignore-gpu-blocklist");
    cmd->AppendSwitch("enable-gpu-rasterization");
    cmd->AppendSwitch("enable-gpu-compositing");
    cmd->AppendSwitch("enable-zero-copy");

    cmd->AppendSwitch("enable-accelerated-video-decode");
    cmd->AppendSwitch("enable-native-gpu-memory-buffers");

    cmd->AppendSwitchWithValue("enable-features", features);
    cmd->AppendSwitchWithValue("disable-features", dis_features);

    // Autoplay video / audio without user-gesture prompts. WE wallpapers
    // routinely auto-play media on load.
    cmd->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");

    // Skip KDE/GNOME password-store probes (kwalletd6 / libsecret /
    // klauncher D-Bus calls). Wallpapers never store credentials.
    cmd->AppendSwitchWithValue("password-store", "basic");

    // Misc.
    cmd->AppendSwitch("no-first-run");
    cmd->AppendSwitch("no-default-browser-check");
    cmd->AppendSwitch("disable-plugins");
    cmd->AppendSwitch("disable-sync");
    cmd->AppendSwitch("disable-translate");
    cmd->AppendSwitch("disable-default-apps");
    cmd->AppendSwitch("disable-extensions");
    cmd->AppendSwitch("disable-client-side-phishing-detection");
    cmd->AppendSwitch("disable-popup-blocking");
    cmd->AppendSwitch("disable-pinch");
    cmd->AppendSwitch("metrics-recording-only");
    cmd->AppendSwitch("disable-component-update");
    cmd->AppendSwitch("disable-session-crashed-bubble");
    cmd->AppendSwitch("disable-search-engine-choice-screen");

    if (m_mute_audio) {
        cmd->AppendSwitch("mute-audio");
    }
}

void AppHandler::OnContextInitialized() {
    // Browser is created from BrowserHost::OpenWallpaper; nothing to do here.
}

void AppHandler::OnContextCreated(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame,
                                  CefRefPtr<CefV8Context> /*context*/) {
    if (! frame || ! frame->IsMain()) return;

    // WE web audio-response API. The page calls wallpaperRegisterAudioListener
    // to subscribe; the browser process feeds samples each tick by invoking
    // __weweb_pushAudio with a 128-float array (64 left + 64 right, 0..1).
    // Installed here so it exists before the page's own scripts run.
    static const char kAudioApi[] =
        "(function(){"
        "  if (window.__weweb_audio_installed) return;"
        "  window.__weweb_audio_installed = true;"
        "  var listeners = [];"
        "  window.wallpaperRegisterAudioListener = function(cb){"
        "    if (typeof cb === 'function') listeners.push(cb);"
        "  };"
        "  window.wallpaperRemoveAudioListener = function(cb){"
        "    var i = listeners.indexOf(cb); if (i >= 0) listeners.splice(i, 1);"
        "  };"
        "  window.__weweb_pushAudio = function(arr){"
        "    for (var i = 0; i < listeners.length; i++){"
        "      try { listeners[i](arr); } catch (e) {}"
        "    }"
        "  };"
        "})();";
    frame->ExecuteJavaScript(kAudioApi, "wallpaper://internal/audio_api.js", 0);

    const auto propertyBridge = BuildPropertyListenerBootstrapSnippet();
    if (! propertyBridge.empty()) {
        frame->ExecuteJavaScript(
            propertyBridge, "wallpaper://internal/property_bridge.js", 0);
    }
}
} // namespace wallpaper
