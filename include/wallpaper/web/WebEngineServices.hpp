#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace wallpaper
{
// ---------------------------------------------------------------------------
// Services the CEF-based web backend needs but cannot create by itself.
// ---------------------------------------------------------------------------
// Mirrors the WESceneEngineServices pattern: the backend's
// CreateWebBackend factory accepts a shared_ptr<WebEngineServices> and fills
// in the missing entries from CreateDefaultWebEngineServices. Tests can
// substitute a fake by providing the same fields with stub functions.
//
// Functions are kept platform-neutral on purpose: the BrowserHost that
// ultimately receives the resources / locales / cache paths is the
// CefSettings adapter (see BrowserHost::Init in commit 5). The audio
// capture fn returns 128 floats (64 left + 64 right, normalised 0..1)
// because that is the layout Wallpaper Engine's web
// wallpaperRegisterAudioListener callback expects when the browser
// process feeds it via __weweb_pushAudio.
struct WebEngineServices {
    // Path to the CEF Resources/ directory (must contain cef.pak,
    // cef_100_percent.pak, cef_200_percent.pak, cef_extensions.pak,
    // devtools_resources.pak, etc.). CefInitialize takes this via
    // settings.resources_dir_path. Return an empty path to leave
    // CefSettings at its default and let CEF discover the resource
    // pack from its own binary directory.
    std::function<std::filesystem::path()> provideCefResourcesDir;

    // Path to the CEF Resources/locales/ directory (must contain
    // en-US.pak and friends). Passed through settings.locales_dir_path.
    std::function<std::filesystem::path()> provideCefLocalesDir;

    // Optional on-disk cache root for CEF. Empty path leaves the CEF
    // default in-memory cache in place. Wallpaper Engine web
    // wallpapers do not benefit from a persistent cache in the
    // daemon case, but a host may want one for development.
    std::function<std::filesystem::path()> provideCefCacheDir;

    // Path to the helper executable CEF should spawn for renderer /
    // utility / zygote child processes.
    std::function<std::filesystem::path()> provideCefSubprocessPath;

    // If true (default), the backend appends --mute-audio to the
    // Chromium command line so no PulseAudio / PipeWire output device
    // is opened. Set false when the host wants the wallpaper to drive
    // the system's default sink (rare; typical setup mutes web audio
    // because the daemon already mixes scene audio).
    std::function<bool()> audioMuted;

    // Pull 128 audio-response samples (64 left + 64 right) suitable
    // for a single __weweb_pushAudio(...) call. The backend calls this
    // every ~33 ms (30 Hz) when the host reports that the page has
    // registered an audio listener. std::nullopt = no samples this
    // tick; the backend will simply skip the JS push and try again
    // next tick. Wallpaper Engine's web convention expects 0..1
    // magnitudes per bin; the host is responsible for any FFT /
    // downsample from its raw capture source.
    std::function<std::optional<std::array<float, 128>>(std::chrono::milliseconds /*period*/)>
        captureAudioSamples;
};

// Return a shared_ptr<WebEngineServices> populated with the host-portable
// defaults: empty CEF resource / locale / cache paths (CEF will resolve
// them from its own binary directory), audio muted, and a no-op audio
    // capture that always returns std::nullopt. Hosts that need different
    // behaviour should build their own services object and call
    // CreateWebBackend(context, services); the default factory is what
// `CreateWebBackend(context)` falls back to when no services object is
// provided.
std::shared_ptr<WebEngineServices> CreateDefaultWebEngineServices();
} // namespace wallpaper
