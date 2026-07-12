#include "wallpaper/abi/WeRenderer.h"
#include "wallpaper/Result.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace
{
constexpr const char* kProjectJson = R"({
  "type": "web",
  "file": "index.html",
  "title": "ABI Input Test"
})";

struct WorkshopFixture {
    std::filesystem::path dir;

    WorkshopFixture() {
        dir = std::filesystem::temp_directory_path()
            / ("we-renderer-input-event-test-" + std::to_string(::getpid()));
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

    we_session_t* session = we_session_create_with_cache_path("/tmp/we-renderer-input-event-test-cache");
    assert(session != nullptr);

    we_source_v1 source {};
    source.size = static_cast<std::uint32_t>(offsetof(we_source_v1, speed));
    source.version = 1;
    source.uri = fixture.dir.c_str();
    source.assets_uri = fixture.dir.c_str();
    source.fps = 30;
    assert(we_session_set_source(session, &source) == 0);

    we_render_config_v1 config {};
    config.size = sizeof(config);
    config.version = 1;
    config.width = 1280;
    config.height = 720;
    config.prefer_dmabuf = true;
    config.allow_shm_fallback = false;
    assert(we_session_set_render_config(session, &config) == 0);

    const auto accepted_backend_result = [](std::int32_t result) {
        return result == 0
            || result == static_cast<std::int32_t>(wallpaper::ResultCode::InvalidState) + 1
            || result == static_cast<std::int32_t>(wallpaper::ResultCode::InternalError) + 1;
    };

    we_input_event_v2 invalid {};
    invalid.size = sizeof(invalid);
    invalid.version = 2;
    invalid.type = 999;
    assert(we_session_send_input_event(session, &invalid) == -1);
    invalid.type = WE_INPUT_POINTER_MOVE;
    invalid.version = 1;
    assert(we_session_send_input_event(session, &invalid) == -1);
    invalid.version = 2;
    invalid.size = sizeof(invalid) - 1;
    assert(we_session_send_input_event(session, &invalid) == -1);

    we_input_event_v2 focus {};
    focus.size = sizeof(focus);
    focus.version = 2;
    focus.type = WE_INPUT_FOCUS;
    focus.focused = true;
    assert(accepted_backend_result(we_session_send_input_event(session, &focus)));
    focus.focused = false;
    assert(accepted_backend_result(we_session_send_input_event(session, &focus)));

    we_input_event_v2 pointer {};
    pointer.size = sizeof(pointer);
    pointer.version = 2;
    pointer.type = WE_INPUT_POINTER_MOVE;
    pointer.pointer_x = 0.5f;
    pointer.pointer_y = 0.5f;
    assert(accepted_backend_result(we_session_send_input_event(session, &pointer)));

    pointer.type = WE_INPUT_POINTER_DOWN;
    pointer.button = 1;
    assert(accepted_backend_result(we_session_send_input_event(session, &pointer)));

    pointer.type = WE_INPUT_POINTER_UP;
    assert(accepted_backend_result(we_session_send_input_event(session, &pointer)));

    we_input_event_v2 wheel {};
    wheel.size = sizeof(wheel);
    wheel.version = 2;
    wheel.type = WE_INPUT_POINTER_WHEEL;
    wheel.pointer_x = 0.25f;
    wheel.pointer_y = 0.75f;
    wheel.wheel_delta_x = -5;
    wheel.wheel_delta_y = 120;
    assert(accepted_backend_result(we_session_send_input_event(session, &wheel)));

    we_input_event_v2 key {};
    key.size = sizeof(key);
    key.version = 2;
    key.type = WE_INPUT_KEY_DOWN;
    key.key_code = 65;
    key.native_key_code = 38;
    key.modifiers = 2;
    key.unicode_char = 'A';
    assert(accepted_backend_result(we_session_send_input_event(session, &key)));
    key.type = WE_INPUT_KEY_UP;
    assert(accepted_backend_result(we_session_send_input_event(session, &key)));

    we_session_destroy(session);
    return 0;
}
