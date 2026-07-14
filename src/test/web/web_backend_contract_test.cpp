#include "api/WallpaperRuntime.hpp"
#include "api/web/Web.hpp"
#include "backend/web/CreateWebBackend.hpp"
#include "backend/web/internal/WebBackend.hpp"
#include "runtime/backend/BackendFactory.hpp"
#include "wallpaper/VulkanOutputInit.hpp"
#include "wallpaper/scene/WESceneContract.hpp"
#include "wallpaper/web/WebOutputBinding.hpp"
#include "test/web/MockWebBrowserHost.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <drm/drm_fourcc.h>

namespace
{
constexpr int kMissingAcceleratedFrameWarningUpdates = 60;

constexpr const char* kProjectJson = R"({
  "type": "web",
  "file": "index.html",
  "title": "Contract Test",
  "general": { "properties": { "color": { "type": "combo", "value": "red" } } }
})";

struct WorkshopFixture {
    std::filesystem::path dir;

    WorkshopFixture() {
        dir = std::filesystem::temp_directory_path() /
              ("wp-web-contract-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        std::ofstream f(dir / "project.json");
        f << kProjectJson;
    }

    ~WorkshopFixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

class SingleBackendFactory final : public wallpaper::BackendFactory {
public:
    explicit SingleBackendFactory(std::unique_ptr<wallpaper::ContentBackend> backend)
        : backend_(std::move(backend)) {}

    wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>>
    create(wallpaper::BackendType, const wallpaper::BackendContext&) override {
        if (! backend_) {
            return wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>>::failure(
                wallpaper::ResultCode::InvalidState, "test backend was already consumed");
        }
        return wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>>::success(
            std::move(backend_));
    }

private:
    std::unique_ptr<wallpaper::ContentBackend> backend_;
};

void requireAt(bool condition, const char* label) {
    if (! condition) {
        std::fprintf(stderr, "web_backend_contract_test: requirement failed at %s\n", label);
        std::abort();
    }
}
} // namespace

int main() {
    WorkshopFixture workshop;

    requireAt(::setenv("WE_CEF_CACHE_DIR", "/tmp/web-backend-contract-env-cache", 1) == 0,
              "set CEF cache environment");
    auto defaultServices = wallpaper::CreateDefaultWebEngineServices();
    requireAt(defaultServices->provideCefCacheDir() == "/tmp/web-backend-contract-env-cache",
              "default service uses CEF cache environment");
    requireAt(::unsetenv("WE_CEF_CACHE_DIR") == 0, "unset CEF cache environment");

    wallpaper::WallpaperRuntime runtime;
    auto                        services = std::make_shared<wallpaper::WebEngineServices>();
    services->provideCefResourcesDir     = []() {
        return std::filesystem::path("/usr/lib/cef");
    };
    services->provideCefLocalesDir = []() {
        return std::filesystem::path("/usr/lib/cef/locales");
    };
    services->provideCefCacheDir = []() {
        return std::filesystem::path("/tmp/web-backend-contract-cache");
    };
    services->provideCefSubprocessPath = []() {
        return std::filesystem::path("/bin/sh");
    };
    services->runtimeProfile = []() {
        return wallpaper::WebCefRuntimeProfile::Compatibility;
    };
    services->preferredWindowSystem = []() {
        return wallpaper::WebCefWindowSystem::X11;
    };
    services->extraCommandLineSwitches = []() {
        return std::vector<std::string> { "foo", "bar=baz" };
    };
    services->audioMuted = []() {
        return true;
    };
    services->captureAudioSamples =
        [](std::chrono::milliseconds) -> std::optional<std::array<float, 128>> {
        return std::nullopt;
    };

    auto rawBackend =
        wallpaper::CreateWebBackend(wallpaper::BackendContext {}, std::move(services));
    requireAt(static_cast<bool>(rawBackend), "raw backend create");
    auto* backend = static_cast<wallpaper::WebBackend*>(rawBackend.value().get());

    // Replace the BrowserHost with a recording fake so the test
    // exercises the WebBackend -> WebBrowserHost contract without
    // touching the CEF runtime.
    auto mock = std::make_shared<wallpaper::test::MockWebBrowserHost>();
    backend->testSetBrowserHost(mock);

    wallpaper::SessionConfig sessionConfig {};
    sessionConfig.backendFactory =
        std::make_shared<SingleBackendFactory>(std::move(rawBackend.value()));
    auto session = runtime.createSession(sessionConfig);
    requireAt(static_cast<bool>(session), "session create");

    // Load: parses the workshop project.json and stores the manifest.
    wallpaper::WebSourceConfig sourceConfig;
    sourceConfig.uri = workshop.dir.string();
    auto loadResult  = session->load(wallpaper::MakeWebWallpaperSource(sourceConfig));
    requireAt(static_cast<bool>(loadResult), "load result");

    // Render config + bind: required before start().
    wallpaper::RenderInitInfo info {};
    info.offscreen                     = true;
    info.export_mode                   = wallpaper::ExternalFrameExportMode::DMA_BUF;
    info.offscreen_tiling              = wallpaper::TexTiling::LINEAR;
    info.allow_shm_fallback            = true;
    info.consumer_dmabuf_formats_known = true;
    info.consumer_dmabuf_formats       = {
        { DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR },
    };
    info.width                      = 640;
    info.height                     = 480;
    auto                    binding = wallpaper::MakeWebOutputBinding(info);
    wallpaper::OutputTarget target {};
    target.type     = wallpaper::OutputTargetType::Offscreen;
    target.binding  = binding;
    target.width    = 640;
    target.height   = 480;
    auto bindResult = session->bindOutput(target);
    requireAt(static_cast<bool>(bindResult), "bind result");

    // Start: begin the CEF lifecycle (Init, OpenWallpaper) — the mock records every call without
    // touching CEF.
    auto playResult = session->play();
    requireAt(static_cast<bool>(playResult), "play result");

    // Init runs before OpenWallpaper.
    requireAt(mock->hasCall("Init"), "mock init call");
    requireAt(mock->hasCall("SetAcceleratedPaintCallback"), "mock accel callback call");
    requireAt(mock->hasCall("SetSoftwarePaintCallback"), "mock software callback call");
    requireAt(mock->hasCall("OpenWallpaper"), "mock open wallpaper call");
    requireAt(mock->has_accelerated_paint_callback, "mock accel callback installed");
    requireAt(mock->has_software_paint_callback, "mock software callback installed");
    assert(mock->last_init_opts.runtime_profile == wallpaper::WebCefRuntimeProfile::Compatibility);
    assert(mock->last_init_opts.preferred_window_system == wallpaper::WebCefWindowSystem::X11);
    assert(mock->last_init_opts.extra_command_line_switches.size() == 2);
    assert(mock->last_init_opts.extra_command_line_switches[0] == "foo");
    assert(mock->last_init_opts.extra_command_line_switches[1] == "bar=baz");
    assert(mock->last_init_opts.prefer_accelerated_paint);

    // The manifest was forwarded verbatim: entry_html is "index.html",
    // the user_props_json round-trips the {color: {type, value}} object.
    assert(mock->last_manifest.entry_html == "index.html");
    assert(mock->last_manifest.has_user_props);
    assert(mock->last_manifest.user_props_json.find("\"color\"") != std::string::npos);

    // OpenWallpaper was sized to the binding's RenderInitInfo.
    assert(mock->last_open_width == 640);
    assert(mock->last_open_height == 480);
    assert(mock->last_workshop_dir == workshop.dir);

    wallpaper::RenderInitInfo resizedInfo = info;
    resizedInfo.width                     = 1024;
    resizedInfo.height                    = 768;
    auto resizedBinding                   = wallpaper::MakeWebOutputBinding(resizedInfo);
    assert(session->bindOutput(wallpaper::MakeWebOutputTarget(resizedBinding)));
    assert(mock->resize_w == 1024);
    assert(mock->resize_h == 768);

    // Start should not advertise a frame until CEF has delivered one.
    auto startupLifecycle = backend->tick();
    assert(startupLifecycle);
    assert(startupLifecycle.value().contentStateChanged);
    assert(! startupLifecycle.value().frameRequested);
    auto diagnostics = backend->diagnostics();
    assert(diagnostics.entries.empty());

    // Audio volume is forwarded through the BrowserHost volume control.
    auto setVol = backend->setProperty(wallpaper::WE_SCENE_PROPERTY_VOLUME, 0.7f);
    assert(setVol);
    assert(mock->last_volume == 0.7f);

    // 30 FPS -> SetFrameRate(30).
    auto setFps =
        backend->setProperty(wallpaper::WE_SCENE_PROPERTY_FPS, static_cast<std::int32_t>(30));
    assert(setFps);
    assert(mock->last_fps == 30);

    // Muting preserves the configured volume so unmute restores it instead of forcing 1.0.
    assert(backend->setProperty(wallpaper::WE_SCENE_PROPERTY_MUTED, true));
    assert(mock->last_volume == 0.0f);
    assert(backend->setProperty(wallpaper::WE_SCENE_PROPERTY_VOLUME, 0.4f));
    assert(mock->last_volume == 0.0f);
    assert(backend->setProperty(wallpaper::WE_SCENE_PROPERTY_MUTED, false));
    assert(mock->last_volume == 0.4f);

    auto audio = std::make_shared<std::vector<float>>(std::initializer_list<float> { 0.1f, 0.2f });
    assert(backend->setProperty(wallpaper::WE_SCENE_PROPERTY_AUDIO_SAMPLES,
                                std::static_pointer_cast<void>(audio)));
    assert(mock->push_audio_count == 1);
    assert(mock->last_audio == *audio);

    auto unsupportedSpeed = backend->setProperty(wallpaper::WE_SCENE_PROPERTY_SPEED, 2.0f);
    assert(! unsupportedSpeed);
    assert(unsupportedSpeed.error().code == wallpaper::ResultCode::NotSupported);

    // Pointer event -> OnMouseMove(x, y) + (on Down) OnMouseButton.
    wallpaper::InputEvent move;
    move.type     = wallpaper::InputEventType::PointerMove;
    move.pointerX = 123.0;
    move.pointerY = 456.0;
    assert(backend->sendInput(move));
    assert(mock->mouse_move_x == 123);
    assert(mock->mouse_move_y == 456);

    wallpaper::InputEvent down;
    down.type     = wallpaper::InputEventType::PointerDown;
    down.pointerX = 100.0;
    down.pointerY = 200.0;
    down.button   = 2;
    assert(backend->sendInput(down));
    assert(mock->mouse_button_x == 100);
    assert(mock->mouse_button_y == 200);
    assert(mock->mouse_button_cef == 2);
    assert(mock->mouse_button_down);

    wallpaper::InputEvent wheel;
    wheel.type        = wallpaper::InputEventType::PointerWheel;
    wheel.pointerX    = 12.0;
    wheel.pointerY    = 34.0;
    wheel.wheelDeltaX = -5;
    wheel.wheelDeltaY = 120;
    assert(backend->sendInput(wheel));
    assert(mock->wheel_x == 12);
    assert(mock->wheel_y == 34);
    assert(mock->wheel_delta_x == -5);
    assert(mock->wheel_delta_y == 120);

    std::vector<std::uint8_t> software_pixels(320u * 240u * 4u, 0x7f);
    mock->software_paint_callback(software_pixels.data(), 320, 240, 320 * 4);
    diagnostics                            = backend->diagnostics();
    std::size_t software_fallback_warnings = 0;
    for (const auto& entry : diagnostics.entries) {
        if (entry.message.find("exported it through SHM fallback") != std::string::npos) {
            ++software_fallback_warnings;
        }
    }
    requireAt(software_fallback_warnings == 1, "single software fallback warning");
    auto software_frame_lifecycle = backend->tick();
    assert(software_frame_lifecycle);
    requireAt(software_frame_lifecycle.value().frameRequested,
              "software paint should request frame");
    auto software_frame_result = resizedBinding->acquireTexture();
    requireAt(software_frame_result.ok(), "software frame should be published");
    auto software_frame = std::move(software_frame_result.value());
    requireAt(software_frame.exportKind == wallpaper::TextureExportKind::SharedMemory,
              "software frame should use shm");
    requireAt(software_frame.extent.width == 320, "software frame width");
    requireAt(software_frame.extent.height == 240, "software frame height");
    requireAt(software_frame.planes[0].descriptor.valid(), "software frame fd");

    mock->software_paint_callback(software_pixels.data(), 320, 240, 320 * 4);
    diagnostics                = backend->diagnostics();
    software_fallback_warnings = 0;
    for (const auto& entry : diagnostics.entries) {
        if (entry.message.find("exported it through SHM fallback") != std::string::npos) {
            ++software_fallback_warnings;
        }
    }
    assert(software_fallback_warnings == 1);

    wallpaper::InputEvent keyDown;
    keyDown.type          = wallpaper::InputEventType::KeyDown;
    keyDown.keyCode       = 65;
    keyDown.nativeKeyCode = 38;
    keyDown.modifiers     = 4;
    keyDown.unicodeChar   = 'A';
    assert(backend->sendInput(keyDown));
    assert(mock->last_key_type == 0);
    assert(mock->last_native_key_code == 38);
    assert(mock->last_windows_key_code == 65);
    assert(mock->last_key_modifiers == 4);
    assert(mock->last_unicode_char == 'A');

    wallpaper::InputEvent focusLost;
    focusLost.type = wallpaper::InputEventType::FocusLost;
    assert(backend->sendInput(focusLost));
    assert(mock->hasCall("OnFocus"));

    // Pause / resume forward to SetPaused.
    assert(session->pause());
    assert(mock->last_paused);
    const auto invalidate_count_before_pause = mock->callCount("Invalidate");
    assert(backend->update());
    assert(mock->callCount("Invalidate") == invalidate_count_before_pause);
    assert(mock->callCount("Pump") >= 1);
    assert(session->play());
    assert(! mock->last_paused);

    for (int i = 0; i < kMissingAcceleratedFrameWarningUpdates; ++i) {
        assert(backend->update());
    }
    diagnostics                              = backend->diagnostics();
    std::size_t missing_accelerated_warnings = 0;
    for (const auto& entry : diagnostics.entries) {
        if (entry.message.find("accelerated paint frames after") != std::string::npos) {
            ++missing_accelerated_warnings;
        }
    }
    requireAt(missing_accelerated_warnings == 0,
              "no missing accel warning when shm fallback is enabled");

    assert(mock->callCount("Pump") >= 1);
    assert(mock->callCount("Invalidate") >= 1);

    // Accelerated paint callback drives real frame availability into the
    // attached WebOutputBinding swapchain.
    const int frame_fd = ::dup(STDOUT_FILENO);
    assert(frame_fd >= 0);
    wallpaper::DmaBufFrame frame {};
    frame.plane_count      = 1;
    frame.planes[0].fd     = frame_fd;
    frame.planes[0].stride = 800 * 4;
    frame.planes[0].offset = 128;
    frame.modifier         = static_cast<std::uint64_t>(DRM_FORMAT_MOD_LINEAR);
    frame.format           = wallpaper::DmaBufFormat::RGBA8_UNORM;
    frame.coded_width      = 800;
    frame.coded_height     = 600;
    frame.visible_x        = 10;
    frame.visible_y        = 20;
    frame.visible_width    = 640;
    frame.visible_height   = 480;
    mock->accelerated_paint_callback(frame);

    auto lifecycle = backend->tick();
    assert(lifecycle);
    assert(lifecycle.value().frameRequested);
    auto ex_frame_result = resizedBinding->acquireTexture();
    assert(ex_frame_result);
    auto ex_frame = std::move(ex_frame_result.value());
    assert(ex_frame.exportKind == wallpaper::TextureExportKind::DmaBuf);
    assert(ex_frame.format == wallpaper::TexturePixelFormat::Rgba8Unorm);
    assert(ex_frame.extent.width == 640);
    assert(ex_frame.extent.height == 480);
    assert(ex_frame.drmFourcc == DRM_FORMAT_ABGR8888);
    assert(ex_frame.planes[0].stride == 800u * 4u);
    assert(ex_frame.planes[0].offset == 128u + 20u * 800u * 4u + 10u * 4u);
    assert(ex_frame.planes[0].descriptor.valid());

    wallpaper::RenderInitInfo incompatibleInfo = resizedInfo;
    incompatibleInfo.consumer_dmabuf_formats   = {
        { DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR },
    };
    auto incompatibleBinding = wallpaper::MakeWebOutputBinding(incompatibleInfo);
    assert(session->bindOutput(wallpaper::MakeWebOutputTarget(incompatibleBinding)));
    mock->accelerated_paint_callback(frame);
    auto incompatibleLifecycle = backend->tick();
    assert(incompatibleLifecycle);
    assert(! incompatibleLifecycle.value().frameRequested);
    assert(! incompatibleBinding->acquireTexture());

    diagnostics                              = backend->diagnostics();
    std::size_t incompatible_format_warnings = 0;
    for (const auto& entry : diagnostics.entries) {
        if (entry.message.find("not advertised by the consumer") != std::string::npos) {
            ++incompatible_format_warnings;
        }
    }
    assert(incompatible_format_warnings == 1);
    mock->accelerated_paint_callback(frame);
    diagnostics                  = backend->diagnostics();
    incompatible_format_warnings = 0;
    for (const auto& entry : diagnostics.entries) {
        if (entry.message.find("not advertised by the consumer") != std::string::npos) {
            ++incompatible_format_warnings;
        }
    }
    assert(incompatible_format_warnings == 1);

    const auto init_count_before_fallback     = mock->callCount("Init");
    const auto open_count_before_fallback     = mock->callCount("OpenWallpaper");
    const auto shutdown_count_before_fallback = mock->callCount("Shutdown");
    assert(backend->update());
    assert(mock->callCount("Init") == init_count_before_fallback);
    assert(mock->callCount("OpenWallpaper") == open_count_before_fallback + 1);
    assert(mock->callCount("Shutdown") == shutdown_count_before_fallback);
    assert(! mock->has_accelerated_paint_callback);

    mock->software_paint_callback(software_pixels.data(), 320, 240, 320 * 4);
    auto fallbackLifecycle = backend->tick();
    assert(fallbackLifecycle);
    assert(fallbackLifecycle.value().frameRequested);
    auto fallbackFrameResult = incompatibleBinding->acquireTexture();
    assert(fallbackFrameResult);
    assert(fallbackFrameResult.value().exportKind == wallpaper::TextureExportKind::SharedMemory);

    wallpaper::RenderInitInfo rejectedInfo = resizedInfo;
    rejectedInfo.allow_shm_fallback        = false;
    rejectedInfo.consumer_dmabuf_formats   = {
        { DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR },
    };
    auto rejectedBinding    = wallpaper::MakeWebOutputBinding(rejectedInfo);
    auto rejectedBindResult = session->bindOutput(wallpaper::MakeWebOutputTarget(rejectedBinding));
    assert(! rejectedBindResult);
    assert(rejectedBindResult.error().code == wallpaper::ResultCode::NotSupported);
    ::close(frame_fd);

    // Stop shuts down CEF once; runtime fallback only reopened the OSR browser.
    assert(session->stop());
    assert(mock->request_close_count == 2);
    assert(mock->callCount("Shutdown") == 1);

    auto missingHelperServices = std::make_shared<wallpaper::WebEngineServices>();
    missingHelperServices->provideCefResourcesDir = []() {
        return std::filesystem::path("/usr/lib/cef");
    };
    missingHelperServices->provideCefLocalesDir = []() {
        return std::filesystem::path("/usr/lib/cef/locales");
    };
    missingHelperServices->provideCefCacheDir = []() {
        return std::filesystem::path("/tmp/web-backend-contract-cache");
    };
    missingHelperServices->provideCefSubprocessPath = []() {
        return std::filesystem::path("/definitely/missing/we-cef-helper");
    };
    missingHelperServices->runtimeProfile = []() {
        return wallpaper::WebCefRuntimeProfile::Compatibility;
    };
    missingHelperServices->preferredWindowSystem = []() {
        return wallpaper::WebCefWindowSystem::Wayland;
    };
    missingHelperServices->extraCommandLineSwitches = []() {
        return std::vector<std::string> {};
    };
    missingHelperServices->audioMuted = []() {
        return true;
    };
    missingHelperServices->captureAudioSamples =
        [](std::chrono::milliseconds) -> std::optional<std::array<float, 128>> {
        return std::nullopt;
    };

    auto missingHelperBackend =
        wallpaper::CreateWebBackend(wallpaper::BackendContext {}, std::move(missingHelperServices));
    requireAt(static_cast<bool>(missingHelperBackend), "missing helper backend create");
    auto* missingHelperRaw =
        static_cast<wallpaper::WebBackend*>(missingHelperBackend.value().get());
    missingHelperRaw->testSetBrowserHost(std::make_shared<wallpaper::test::MockWebBrowserHost>());

    wallpaper::RenderInitInfo missingInfo {};
    missingInfo.offscreen                  = true;
    missingInfo.export_mode                = wallpaper::ExternalFrameExportMode::DMA_BUF;
    missingInfo.offscreen_tiling           = wallpaper::TexTiling::LINEAR;
    missingInfo.width                      = 640;
    missingInfo.height                     = 480;
    auto                    missingBinding = wallpaper::MakeWebOutputBinding(missingInfo);
    wallpaper::OutputTarget missingTarget {};
    missingTarget.type    = wallpaper::OutputTargetType::Offscreen;
    missingTarget.binding = missingBinding;
    missingTarget.width   = 640;
    missingTarget.height  = 480;

    wallpaper::SessionConfig missingHelperSessionConfig {};
    missingHelperSessionConfig.backendFactory =
        std::make_shared<SingleBackendFactory>(std::move(missingHelperBackend.value()));
    auto missingHelperSession = runtime.createSession(missingHelperSessionConfig);
    requireAt(static_cast<bool>(missingHelperSession), "missing helper session create");

    auto missingLoadResult =
        missingHelperSession->load(wallpaper::MakeWebWallpaperSource(sourceConfig));
    requireAt(static_cast<bool>(missingLoadResult), "missing helper load");
    auto missingBindResult = missingHelperSession->bindOutput(missingTarget);
    requireAt(static_cast<bool>(missingBindResult), "missing helper bind");
    auto missingPlayResult = missingHelperSession->play();
    requireAt(! missingPlayResult, "missing helper play should fail");
    const auto missingDiagnostics = missingHelperRaw->diagnostics();
    requireAt(! missingDiagnostics.entries.empty(), "missing helper diagnostics not empty");
    bool sawMissingHelperDiagnostic = false;
    for (const auto& entry : missingDiagnostics.entries) {
        if (entry.message.find("CEF subprocess helper not found") != std::string::npos) {
            sawMissingHelperDiagnostic = true;
            break;
        }
    }
    requireAt(sawMissingHelperDiagnostic, "missing helper diagnostic content");

    return 0;
}
