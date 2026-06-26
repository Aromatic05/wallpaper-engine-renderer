#include "wallpaper/abi/WeRenderer.h"
#include "wallpaper/Result.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace
{
constexpr const char* kProjectJson = R"({
  "type": "web",
  "file": "index.html",
  "title": "ABI Web Source Test"
})";

struct WorkshopFixture {
    std::filesystem::path dir;

    WorkshopFixture() {
        dir = std::filesystem::temp_directory_path()
            / ("we-renderer-web-source-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        std::ofstream project(dir / "project.json");
        project << kProjectJson;
        std::ofstream html(dir / "index.html");
        html << "<!doctype html><title>ok</title>";
    }

    ~WorkshopFixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
} // namespace

int main() {
    WorkshopFixture fixture;

    we_session_t* session = we_session_create_with_cache_path("/tmp/we-renderer-web-source-test-cache");
    assert(session != nullptr);

    we_source_v1 source {};
    source.size = static_cast<std::uint32_t>(offsetof(we_source_v1, speed));
    source.version = 1;
    source.uri = fixture.dir.c_str();
    source.assets_uri = fixture.dir.c_str();
    source.fps = 30;

    const std::int32_t set_source_result = we_session_set_source(session, &source);
    assert(set_source_result == 0);

    we_render_config_v1 unsupported_config {};
    unsupported_config.size = sizeof(unsupported_config);
    unsupported_config.version = 1;
    unsupported_config.width = 1280;
    unsupported_config.height = 720;
    unsupported_config.prefer_dmabuf = false;
    unsupported_config.allow_shm_fallback = true;
    const std::int32_t unsupported_result = we_session_set_render_config(session, &unsupported_config);
    assert(unsupported_result == static_cast<std::int32_t>(wallpaper::ResultCode::NotSupported) + 1);

    we_render_config_v1 supported_config {};
    supported_config.size = sizeof(supported_config);
    supported_config.version = 1;
    supported_config.width = 1280;
    supported_config.height = 720;
    supported_config.prefer_dmabuf = true;
    supported_config.allow_shm_fallback = false;
    const std::int32_t supported_result = we_session_set_render_config(session, &supported_config);
    assert(supported_result == 0);

    we_session_destroy(session);
    return 0;
}
