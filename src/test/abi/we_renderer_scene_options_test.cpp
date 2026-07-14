#include "wallpaper/abi/WeRenderer.h"
#include "wallpaper/Result.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace
{
struct SceneFixture {
    std::filesystem::path dir;

    SceneFixture() {
        dir = std::filesystem::temp_directory_path() /
              ("we-renderer-scene-options-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        std::ofstream project(dir / "project.json");
        project << R"({"type":"scene","file":"scene.json","title":"ABI Scene Options Test"})";
    }

    ~SceneFixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
std::string ReadDiagnostics(we_session_t* session) {
    uint32_t size = 0;
    assert(we_session_get_diagnostics_json(session, nullptr, &size) == 0);
    assert(size > 1);

    std::vector<char> tooSmall(size - 1);
    uint32_t          tooSmallSize = static_cast<uint32_t>(tooSmall.size());
    assert(we_session_get_diagnostics_json(session, tooSmall.data(), &tooSmallSize) == -2);
    assert(tooSmallSize == size);

    std::vector<char> buffer(size);
    uint32_t          bufferSize = static_cast<uint32_t>(buffer.size());
    assert(we_session_get_diagnostics_json(session, buffer.data(), &bufferSize) == 0);
    assert(bufferSize == size);
    return std::string(buffer.data());
}

} // namespace

int main() {
    SceneFixture fixture;

    we_session_t* session = we_session_create();
    assert(session != nullptr);

    const std::string graphPath = (fixture.dir / "scene.dot").string();
    const std::string options   = std::string(R"({
        "version": 1,
        "scene": {
            "userProperties": {
                "enabled": true,
                "scale": 1.25,
                "title": "clock"
            },
            "graphviz": {
                "enabled": true,
                "path": ")") + graphPath +
                                  R"("
            }
        }
    })";

    we_source_v1 source {};
    source.size         = sizeof(source);
    source.version      = 1;
    source.uri          = fixture.dir.c_str();
    source.assets_uri   = fixture.dir.c_str();
    source.fps          = 30;
    source.speed        = 1.0f;
    source.volume       = 1.0f;
    source.options_json = options.c_str();
    assert(we_session_set_source(session, &source) == 0);

    we_render_config_v1 renderConfig {};
    renderConfig.size               = sizeof(renderConfig);
    renderConfig.version            = 1;
    renderConfig.width              = 64;
    renderConfig.height             = 64;
    renderConfig.prefer_dmabuf      = true;
    renderConfig.allow_shm_fallback = true;
    assert(we_session_set_render_config(session, &renderConfig) == 0);
    // A compositor can publish its exact DMA-BUF feedback after the initial output bind.
    // Rebinding while the scene loader is active must detach the old swapchain before replacement.
    assert(we_session_set_dmabuf_formats(session, nullptr, nullptr, 0) == 0);

    we_runtime_settings_v1 runtimeSettings {};
    runtimeSettings.size      = sizeof(runtimeSettings);
    runtimeSettings.version   = 1;
    runtimeSettings.fields    = WE_RUNTIME_SETTINGS_FPS | WE_RUNTIME_SETTINGS_SPEED |
                                WE_RUNTIME_SETTINGS_VOLUME | WE_RUNTIME_SETTINGS_MUTED |
                                WE_RUNTIME_SETTINGS_FILL_MODE;
    runtimeSettings.fps       = 60;
    runtimeSettings.speed     = 1.5f;
    runtimeSettings.volume    = 0.4f;
    runtimeSettings.muted     = false;
    runtimeSettings.fill_mode = WE_FILL_MODE_ASPECT_FIT;
    assert(we_session_apply_runtime_settings(session, &runtimeSettings) == 0);

    const float audioSamples[] { 0.1f, 0.2f, 0.3f, 0.4f };
    assert(we_session_push_audio_samples(session, audioSamples, 4) == 0);

    we_media_state_v1 mediaState {};
    mediaState.size           = sizeof(mediaState);
    mediaState.version        = 1;
    mediaState.playback_state = 1;
    mediaState.title          = "ABI media title";
    mediaState.artist         = "ABI media artist";
    assert(we_session_set_media_state(session, &mediaState) == 0);

    runtimeSettings.fps = 4;
    assert(we_session_apply_runtime_settings(session, &runtimeSettings) == -1);

    assert(we_session_set_user_properties_json(
               session, R"({"enabled":false,"scale":2.0,"title":"updated"})") == 0);
    assert(we_session_set_user_properties_json(session, R"({"bad":null})") ==
           static_cast<std::int32_t>(wallpaper::ResultCode::InvalidArgument) + 1);
    assert(we_session_set_user_properties_json(session, "[]") ==
           static_cast<std::int32_t>(wallpaper::ResultCode::InvalidArgument) + 1);

    const auto liveDiagnostics = ReadDiagnostics(session);
    assert(liveDiagnostics.find("\"version\":1") != std::string::npos);
    assert(liveDiagnostics.find("abi.user-properties") != std::string::npos);
    assert(liveDiagnostics.find("must be a JSON object") != std::string::npos);

    we_session_destroy(session);

    we_session_t* invalidSession = we_session_create();
    assert(invalidSession != nullptr);
    const char* invalidOptions = R"({"version":2})";
    source.options_json        = invalidOptions;
    assert(we_session_set_source(invalidSession, &source) ==
           static_cast<std::int32_t>(wallpaper::ResultCode::NotSupported) + 1);
    const auto invalidDiagnostics = ReadDiagnostics(invalidSession);
    assert(invalidDiagnostics.find("abi.source.options") != std::string::npos);
    assert(invalidDiagnostics.find("unsupported source options version") != std::string::npos);
    we_session_destroy(invalidSession);

    return 0;
}
