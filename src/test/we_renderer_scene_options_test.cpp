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
struct SceneFixture {
    std::filesystem::path dir;

    SceneFixture() {
        dir = std::filesystem::temp_directory_path()
            / ("we-renderer-scene-options-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        std::ofstream project(dir / "project.json");
        project << R"({"type":"scene","file":"scene.json","title":"ABI Scene Options Test"})";
    }

    ~SceneFixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
} // namespace

int main() {
    SceneFixture fixture;

    we_session_t* session = we_session_create();
    assert(session != nullptr);

    const std::string graphPath = (fixture.dir / "scene.dot").string();
    const std::string options = std::string(R"({
        "version": 1,
        "scene": {
            "userProperties": {
                "enabled": true,
                "scale": 1.25,
                "title": "clock"
            },
            "graphviz": {
                "enabled": true,
                "path": ")") + graphPath + R"("
            }
        }
    })";

    we_source_v1 source {};
    source.size = sizeof(source);
    source.version = 1;
    source.uri = fixture.dir.c_str();
    source.assets_uri = fixture.dir.c_str();
    source.fps = 30;
    source.speed = 1.0f;
    source.volume = 1.0f;
    source.options_json = options.c_str();
    assert(we_session_set_source(session, &source) == 0);

    assert(we_session_set_user_properties_json(
               session, R"({"enabled":false,"scale":2.0,"title":"updated"})")
           == 0);
    assert(we_session_set_user_properties_json(session, R"({"bad":null})")
           == static_cast<std::int32_t>(wallpaper::ResultCode::InvalidArgument) + 1);
    assert(we_session_set_user_properties_json(session, "[]")
           == static_cast<std::int32_t>(wallpaper::ResultCode::InvalidArgument) + 1);

    we_session_destroy(session);

    we_session_t* invalidSession = we_session_create();
    assert(invalidSession != nullptr);
    const char* invalidOptions = R"({"version":2})";
    source.options_json = invalidOptions;
    assert(we_session_set_source(invalidSession, &source)
           == static_cast<std::int32_t>(wallpaper::ResultCode::NotSupported) + 1);
    we_session_destroy(invalidSession);

    return 0;
}
