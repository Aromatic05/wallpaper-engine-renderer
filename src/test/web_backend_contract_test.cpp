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
constexpr const char* kProjectJson = R"({
  "type": "web",
  "file": "index.html",
  "title": "Contract Test",
  "general": { "properties": { "color": { "type": "combo", "value": "red" } } }
})";

struct WorkshopFixture {
    std::filesystem::path dir;

    WorkshopFixture() {
        dir = std::filesystem::temp_directory_path()
            / ("wp-web-contract-test-" + std::to_string(::getpid()));
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

    wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>> create(
        wallpaper::BackendType, const wallpaper::BackendContext&) override {
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

void require(bool condition) {
    if (! condition) {
        std::fprintf(stderr, "web_backend_contract_test: requirement failed\n");
        std::abort();
    }
}
} // namespace

int main() {
    WorkshopFixture workshop;

    wallpaper::WallpaperRuntime runtime;
    auto rawBackend = wallpaper::CreateWebBackend(wallpaper::BackendContext {});
    require(static_cast<bool>(rawBackend));
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
    require(static_cast<bool>(session));

    // Load: parses the workshop project.json and stores the manifest.
    wallpaper::WebSourceConfig sourceConfig;
    sourceConfig.uri = workshop.dir.string();
    auto loadResult = session->load(wallpaper::MakeWebWallpaperSource(sourceConfig));
    require(static_cast<bool>(loadResult));

    // Render config + bind: required before start().
    wallpaper::RenderInitInfo info {};
    info.offscreen = true;
    info.export_mode = wallpaper::ExternalFrameExportMode::DMA_BUF;
    info.offscreen_tiling = wallpaper::TexTiling::LINEAR;
    info.width = 640;
    info.height = 480;
    auto binding = wallpaper::MakeWebOutputBinding(info);
    wallpaper::OutputTarget target {};
    target.type = wallpaper::OutputTargetType::Offscreen;
    target.binding = binding;
    target.width = 640;
    target.height = 480;
    auto bindResult = session->bindOutput(target);
    require(static_cast<bool>(bindResult));

    // Start: begin the CEF lifecycle (Init, OpenWallpaper) — the mock records every call without
    // touching CEF.
    auto playResult = session->play();
    require(static_cast<bool>(playResult));

    // Init runs before OpenWallpaper.
    require(mock->hasCall("Init"));
    require(mock->hasCall("SetAcceleratedPaintCallback"));
    require(mock->hasCall("OpenWallpaper"));
    require(mock->has_accelerated_paint_callback);

    // The manifest was forwarded verbatim: entry_html is "index.html",
    // the user_props_json round-trips the {color: {type, value}} object.
    assert(mock->last_manifest.entry_html == "index.html");
    assert(mock->last_manifest.has_user_props);
    assert(mock->last_manifest.user_props_json.find("\"color\"") != std::string::npos);

    // OpenWallpaper was sized to the binding's RenderInitInfo.
    assert(mock->last_open_width == 640);
    assert(mock->last_open_height == 480);
    assert(mock->last_workshop_dir == workshop.dir);

    // Start should not advertise a frame until CEF has delivered one.
    auto startupLifecycle = backend->tick();
    assert(startupLifecycle);
    assert(startupLifecycle.value().contentStateChanged);
    assert(! startupLifecycle.value().frameRequested);

    // Audio volume 0.7 lands on ApplyVolume(0.7). ApplyVolume builds
    // the applyUserProperties({audio: {value: 0.7}}) snippet, so the
    // JS-side ApplyUserProperty call is expected too.
    auto setVol = backend->setProperty(wallpaper::WE_SCENE_PROPERTY_VOLUME, 0.7f);
    assert(setVol);
    assert(mock->last_volume == 0.7f);
    assert(mock->hasCall("ApplyUserProperty"));
    assert(mock->last_user_key == "audio");
    assert(mock->last_user_value_json.find("0.7000") != std::string::npos);

    // 30 FPS -> SetFrameRate(30).
    auto setFps = backend->setProperty(wallpaper::WE_SCENE_PROPERTY_FPS, static_cast<std::int32_t>(30));
    assert(setFps);
    assert(mock->last_fps == 30);

    // Pointer event -> OnMouseMove(x, y) + (on Down) OnMouseButton.
    wallpaper::InputEvent move;
    move.type = wallpaper::InputEventType::PointerMove;
    move.pointerX = 123.0;
    move.pointerY = 456.0;
    assert(backend->sendInput(move));
    assert(mock->mouse_move_x == 123);
    assert(mock->mouse_move_y == 456);

    wallpaper::InputEvent down;
    down.type = wallpaper::InputEventType::PointerDown;
    down.pointerX = 100.0;
    down.pointerY = 200.0;
    down.button = 2;
    assert(backend->sendInput(down));
    assert(mock->mouse_button_x == 100);
    assert(mock->mouse_button_y == 200);
    assert(mock->mouse_button_cef == 2);
    assert(mock->mouse_button_down);

    wallpaper::InputEvent wheel;
    wheel.type = wallpaper::InputEventType::PointerWheel;
    wheel.pointerX = 12.0;
    wheel.pointerY = 34.0;
    wheel.wheelDeltaX = -5;
    wheel.wheelDeltaY = 120;
    assert(backend->sendInput(wheel));
    assert(mock->wheel_x == 12);
    assert(mock->wheel_y == 34);
    assert(mock->wheel_delta_x == -5);
    assert(mock->wheel_delta_y == 120);

    wallpaper::InputEvent keyDown;
    keyDown.type = wallpaper::InputEventType::KeyDown;
    keyDown.keyCode = 65;
    keyDown.nativeKeyCode = 38;
    keyDown.modifiers = 4;
    keyDown.unicodeChar = 'A';
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
    assert(session->update());
    assert(mock->callCount("Invalidate") == invalidate_count_before_pause);
    assert(mock->callCount("Pump") >= 1);
    assert(session->play());
    assert(! mock->last_paused);

    // Update -> Invalidate + Pump while active.
    assert(session->update());
    assert(mock->callCount("Pump") >= 1);
    assert(mock->callCount("Invalidate") >= 1);

    // Accelerated paint callback drives real frame availability into the
    // attached WebOutputBinding swapchain.
    const int frame_fd = ::dup(STDOUT_FILENO);
    assert(frame_fd >= 0);
    wallpaper::DmaBufFrame frame {};
    frame.plane_count = 1;
    frame.planes[0].fd = frame_fd;
    frame.planes[0].stride = 800 * 4;
    frame.planes[0].offset = 128;
    frame.modifier = static_cast<std::uint64_t>(DRM_FORMAT_MOD_LINEAR);
    frame.format = wallpaper::DmaBufFormat::RGBA8_UNORM;
    frame.coded_width = 800;
    frame.coded_height = 600;
    frame.visible_x = 10;
    frame.visible_y = 20;
    frame.visible_width = 640;
    frame.visible_height = 480;
    mock->accelerated_paint_callback(frame);

    auto lifecycle = backend->tick();
    assert(lifecycle);
    assert(lifecycle.value().frameRequested);
    assert(binding->swapchain() != nullptr);
    auto* ex_frame = binding->swapchain()->eatFrame();
    assert(ex_frame != nullptr);
    assert(ex_frame->isDmabuf());
    assert(ex_frame->width == 640);
    assert(ex_frame->height == 480);
    assert(ex_frame->drm_fourcc == DRM_FORMAT_ABGR8888);
    assert(ex_frame->planes[0].stride == 800u * 4u);
    assert(ex_frame->planes[0].offset == 128u + 20u * 800u * 4u + 10u * 4u);
    assert(ex_frame->planes[0].fd >= 0);
    ::close(frame_fd);

    // Stop -> Shutdown.
    assert(session->stop());
    assert(mock->request_close_count == 1);
    assert(mock->callCount("Shutdown") == 1);

    return 0;
}
