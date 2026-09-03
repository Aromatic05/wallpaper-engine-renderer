// Optional smoke test against a real CEF runtime. Off by default; the
// CMakeLists opt-in WP_ENABLE_CEF_INTEGRATION_TEST=ON builds this
// target only when BUILD_WEWEB=ON is also set. The test:
//
//   1. Probes the host for a CEF runtime (libcef.so + Resources/
//      locales/) and CEF_ROOT. If either is missing, the test
//      skips with a one-line diagnostic instead of failing — this
//      keeps CI green when no CEF is staged.
//   2. Loads a workshop dir from WE_WEB_TEST_WORKSHOP_DIR when set,
//      otherwise writes a minimal workshop dir into a temp directory.
//   3. Constructs a WebBackend through the BuiltinBackendFactory,
//      drives load / bind / play, and pumps CefDoMessageLoopWork
//      a few times to make sure CEF does not deadlock or SIGABRT
//      on a trivial page.
//   4. Tears the session down via stop and asserts Shutdown ran.
//
// This is a smoke test only — it does not validate the DMA-BUF
// frame path (which needs a Vulkan allocator and would not work
// in a headless CI). The mock contract test in
// web_backend_contract_test.cpp covers the backend -> BrowserHost
// contract; this test covers the BrowserHost -> CEF runtime contract
// at the "doesn't crash" level.

#include "api/WallpaperRuntime.hpp"
#include "api/web/Web.hpp"
#include "backend/BuiltinSessionFactory.hpp"
#include "backend/web/CreateWebBackend.hpp"
#include "wallpaper/web/WebOutputBinding.hpp"
#include "wallpaper/OutputTarget.hpp"

#include <cstdio>
#include <cstdlib>
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
  "title": "Integration Smoke"
})";

constexpr const char* kIndexHtml = R"(<!doctype html>
<html><head><title>smoke</title></head>
<body><script>document.title = "ok";</script></body></html>
)";

struct WorkshopFixture {
    std::filesystem::path dir;
    bool                  owns_dir { true };

    WorkshopFixture() {
        if (const char* configured = std::getenv("WE_WEB_TEST_WORKSHOP_DIR")) {
            if (configured[0]) {
                dir = configured;
                owns_dir = false;
                return;
            }
        }

        dir = std::filesystem::temp_directory_path()
            / ("wp-web-cef-integration-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        std::ofstream pj(dir / "project.json");
        pj << kProjectJson;
        std::ofstream ih(dir / "index.html");
        ih << kIndexHtml;
    }

    ~WorkshopFixture() {
        if (! owns_dir) return;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

bool hasWorkshopManifest(const std::filesystem::path& dir) {
    return std::filesystem::exists(dir / "project.json")
        && std::filesystem::is_regular_file(dir / "project.json");
}

bool hasCefRuntime() {
    if (const char* root = std::getenv("CEF_ROOT")) {
        if (! root[0]) return false;
        std::filesystem::path p { root };
        if (! std::filesystem::exists(p / "libcef.so")) return false;
        if (! std::filesystem::exists(p / "Resources" / "locales" / "en-US.pak")
            && ! std::filesystem::exists(p / "locales" / "en-US.pak")) {
            return false;
        }
        return true;
    }
    return (std::filesystem::exists("/usr/lib/libcef.so")
            && std::filesystem::exists("/usr/lib/cef/locales/en-US.pak"))
        || (std::filesystem::exists("/usr/lib64/libcef.so")
            && std::filesystem::exists("/usr/lib64/cef/locales/en-US.pak"))
        || (std::filesystem::exists("/usr/lib/cef/libcef.so")
            && std::filesystem::exists("/usr/lib/cef/locales/en-US.pak"));
}
} // namespace

int main() {
    if (! hasCefRuntime()) {
        std::fprintf(stderr,
                     "web_cef_integration_test: CEF runtime not present on this host; "
                     "set CEF_ROOT (and stage libcef.so + Resources/locales) to enable.\n");
        return 0; // skip rather than fail
    }

    WorkshopFixture workshop;
    if (! hasWorkshopManifest(workshop.dir)) {
        std::fprintf(stderr,
                     "web_cef_integration_test: workshop dir has no project.json: %s\n",
                     workshop.dir.string().c_str());
        return 7;
    }

    wallpaper::WallpaperRuntime runtime;

    // CEF is process-global while wallpaper sessions are restartable. Exercise repeated
    // init/play/stop lifecycles in one process so an accidental CefShutdown between sessions
    // is caught by the next cycle instead of being hidden by process exit.
    for (int cycle = 0; cycle < 4; ++cycle) {
        auto session = wallpaper::CreateBuiltinSession(runtime, {});
        if (! session) return 1;

        wallpaper::WebSourceConfig config;
        config.uri = workshop.dir.string();
        if (! session->load(wallpaper::MakeWebWallpaperSource(config))) return 2;

        wallpaper::RenderInitInfo info {};
        info.offscreen        = true;
        info.export_mode      = wallpaper::ExternalFrameExportMode::DMA_BUF;
        info.offscreen_tiling = wallpaper::TexTiling::LINEAR;
        info.width            = 320;
        info.height           = 240;
        auto binding = wallpaper::MakeWebOutputBinding(info);
        wallpaper::OutputTarget target {};
        target.type    = wallpaper::OutputTargetType::Offscreen;
        target.binding = binding;
        target.width   = 320;
        target.height  = 240;
        if (! session->bindOutput(target)) return 3;

        if (! session->play()) return 4;
        for (int i = 0; i < 30; ++i) {
            if (! session->tick()) return 5;
        }
        if (! session->stop()) return 6;
    }

    return 0;
}
