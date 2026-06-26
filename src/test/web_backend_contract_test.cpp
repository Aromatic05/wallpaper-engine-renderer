#include "api/WallpaperRuntime.hpp"
#include "api/web/Web.hpp"
#include "backend/BuiltinSessionFactory.hpp"
#include "backend/web/CreateWebBackend.hpp"
#include "backend/web/internal/WebBackend.hpp"
#include "test/web/MockWebBrowserHost.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

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
} // namespace

int main() {
    WorkshopFixture workshop;

    wallpaper::WallpaperRuntime runtime;
    auto                        session = wallpaper::CreateBuiltinSession(runtime, {});
    assert(session);

    // The BuiltinBackendFactory's Web case requires a real WebBackend
    // to be available (i.e. -DBUILD_WEWEB=ON). The CMake gate on the
    // test target guarantees this.
    auto rawBackend = wallpaper::CreateWebBackend(wallpaper::BackendContext {});
    assert(rawBackend);
    auto* backend = static_cast<wallpaper::WebBackend*>(rawBackend.value().get());

    // Replace the BrowserHost with a recording fake so the test
    // exercises the WebBackend -> WebBrowserHost contract without
    // touching the CEF runtime.
    auto mock = std::make_shared<wallpaper::test::MockWebBrowserHost>();
    backend->testSetBrowserHost(mock);

    // Load: parses the workshop project.json and stores the manifest.
    wallpaper::WebSourceConfig config;
    config.uri = workshop.dir.string();
    auto loadResult = session->load(wallpaper::MakeWebWallpaperSource(config));
    assert(loadResult);

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
    assert(bindResult);

    // Start: begin the CEF lifecycle (RunOrExitIfHelper, Init,
    // OpenWallpaper) — but the mock records every call without
    // touching CEF.
    auto playResult = session->play();
    assert(playResult);

    // RunOrExitIfHelper was the first BrowserHost call (returns -1 so
    // the main process continues). Init follows, then OpenWallpaper.
    assert(mock->hasCall("RunOrExitIfHelper"));
    assert(mock->hasCall("Init"));
    assert(mock->hasCall("OpenWallpaper"));

    // The manifest was forwarded verbatim: entry_html is "index.html",
    // the user_props_json round-trips the {color: {type, value}} object.
    assert(mock->last_manifest.entry_html == "index.html");
    assert(mock->last_manifest.has_user_props);
    assert(mock->last_manifest.user_props_json.find("\"color\"") != std::string::npos);

    // OpenWallpaper was sized to the binding's RenderInitInfo.
    assert(mock->last_open_width == 640);
    assert(mock->last_open_height == 480);
    assert(mock->last_workshop_dir == workshop.dir);

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
    assert(backend->sendInput(down));
    assert(mock->mouse_button_x == 100);
    assert(mock->mouse_button_y == 200);
    assert(mock->mouse_button_down);

    // Pause / resume forward to SetPaused.
    assert(session->pause());
    assert(mock->last_paused);
    assert(session->play());
    assert(! mock->last_paused);

    // Update -> Pump.
    assert(session->update());
    assert(mock->callCount("Pump") >= 1);

    // Stop -> Shutdown.
    assert(session->stop());
    assert(mock->callCount("Shutdown") == 1);

    return 0;
}
