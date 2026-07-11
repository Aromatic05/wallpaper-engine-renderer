#include "abi/WeProjectSource.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
namespace fs = std::filesystem;

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr, "project source test failure: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

struct TempProject {
    fs::path dir;

    explicit TempProject(std::string_view projectJson) {
        dir = fs::temp_directory_path()
            / ("we-project-source-test-" + std::to_string(::getpid()) + "-"
               + std::to_string(++counter));
        fs::create_directories(dir);
        std::ofstream output(dir / "project.json");
        output << projectJson;
    }

    ~TempProject() {
        std::error_code error;
        fs::remove_all(dir, error);
    }

    static inline int counter = 0;
};

void TestSceneDefaults() {
    TempProject project(R"({"type":"scene"})");
    auto result = wallpaper::ResolveProjectSource(project.dir.string());
    Require(result.ok(), "scene project without file should resolve");
    Require(result.value().type == wallpaper::BackendType::WEScene,
            "scene backend type mismatch");
    Require(result.value().backendUri == (project.dir / "scene.pkg").string(),
            "scene default package path mismatch");
}

void TestNonStandardSceneNames() {
    {
        TempProject project(R"({"type":"scene","file":"custom.json"})");
        auto result = wallpaper::ResolveProjectSource(project.dir.string());
        Require(result.ok(), "custom scene json should resolve");
        Require(result.value().backendUri == (project.dir / "custom.pkg").string(),
                "custom json should map to matching pkg");
    }
    {
        TempProject project(R"({"type":"SCENE","file":"nested/custom.pkg"})");
        auto result = wallpaper::ResolveProjectSource((project.dir / "project.json").string());
        Require(result.ok(), "custom scene pkg should resolve from project.json path");
        Require(result.value().backendUri == (project.dir / "nested/custom.pkg").string(),
                "custom pkg path mismatch");
    }
}

void TestOtherBackends() {
    {
        TempProject project(R"({"type":"web","file":"index.html"})");
        auto result = wallpaper::ResolveProjectSource(project.dir.string());
        Require(result.ok() && result.value().type == wallpaper::BackendType::Web,
                "web project should resolve");
        Require(result.value().backendUri == project.dir.string(),
                "web backend uri should remain the project directory");
    }
    {
        TempProject project(R"({"type":"video","file":"media/video.mp4"})");
        auto result = wallpaper::ResolveProjectSource(project.dir.string());
        Require(result.ok() && result.value().type == wallpaper::BackendType::Video,
                "video project should resolve");
        Require(result.value().backendUri == (project.dir / "media/video.mp4").string(),
                "video backend uri mismatch");
    }
}

void TestInvalidProjects() {
    {
        TempProject project(R"({"type":"scene","file":"../outside.json"})");
        auto result = wallpaper::ResolveProjectSource(project.dir.string());
        Require(! result && result.error().code == wallpaper::ResultCode::InvalidArgument,
                "scene path traversal must fail");
    }
    {
        TempProject project(R"({"type":"video","file":"/tmp/outside.mp4"})");
        auto result = wallpaper::ResolveProjectSource(project.dir.string());
        Require(! result && result.error().code == wallpaper::ResultCode::InvalidArgument,
                "absolute video path must fail");
    }
    {
        TempProject project(R"({"type":"scene","file":7})");
        auto result = wallpaper::ResolveProjectSource(project.dir.string());
        Require(! result && result.error().code == wallpaper::ResultCode::InvalidArgument,
                "non-string scene file must fail");
    }
    {
        TempProject project(R"({"type":"unsupported"})");
        auto result = wallpaper::ResolveProjectSource(project.dir.string());
        Require(! result && result.error().code == wallpaper::ResultCode::NotSupported,
                "unknown project type must fail");
    }
}
} // namespace

int main() {
    TestSceneDefaults();
    TestNonStandardSceneNames();
    TestOtherBackends();
    TestInvalidProjects();
    return 0;
}
