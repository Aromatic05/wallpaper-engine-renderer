#include "wallpaper/abi/WeRenderer.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace
{
constexpr const char* kProjectJson = R"({
  "type": "video",
  "file": "clip.mp4",
  "title": "ABI Video Source Test"
})";

struct WorkshopFixture {
    std::filesystem::path dir;

    WorkshopFixture() {
        dir = std::filesystem::temp_directory_path()
            / ("we-renderer-video-source-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        std::ofstream project(dir / "project.json");
        project << kProjectJson;
        std::ofstream clip(dir / "clip.mp4", std::ios::binary);
        clip << "not-a-real-video";
    }

    ~WorkshopFixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
} // namespace

int main() {
    WorkshopFixture fixture;

    we_session_t* session = we_session_create_with_cache_path("/tmp/we-renderer-video-source-test-cache");
    assert(session != nullptr);

    we_source_v1 source {};
    source.size = static_cast<std::uint32_t>(sizeof(source));
    source.version = 1;
    source.uri = fixture.dir.c_str();

    const std::int32_t set_source_result = we_session_set_source(session, &source);
    assert(set_source_result == 0);

    we_render_config_v1 dmabuf_config {};
    dmabuf_config.size = sizeof(dmabuf_config);
    dmabuf_config.version = 1;
    dmabuf_config.width = 1280;
    dmabuf_config.height = 720;
    dmabuf_config.prefer_dmabuf = true;
    dmabuf_config.allow_shm_fallback = false;
    assert(we_session_set_render_config(session, &dmabuf_config) == 0);

    we_render_config_v1 fallback_config {};
    fallback_config.size = sizeof(fallback_config);
    fallback_config.version = 1;
    fallback_config.width = 1280;
    fallback_config.height = 720;
    fallback_config.prefer_dmabuf = true;
    fallback_config.allow_shm_fallback = true;
    assert(we_session_set_render_config(session, &fallback_config) == 0);

    we_render_config_v1 shm_config {};
    shm_config.size = sizeof(shm_config);
    shm_config.version = 1;
    shm_config.width = 1280;
    shm_config.height = 720;
    shm_config.prefer_dmabuf = false;
    shm_config.allow_shm_fallback = true;
    assert(we_session_set_render_config(session, &shm_config) == 0);

    we_session_destroy(session);
    return 0;
}