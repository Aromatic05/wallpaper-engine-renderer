#include "wallpaper/abi/WeRenderer.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>

namespace
{
constexpr const char* kProjectJson = R"({"type":"video","file":"clip.mp4","title":"Video Pipeline Test"})";

constexpr std::string_view kFixtureMp4Base64 =
    "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";

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
}

std::vector<std::uint8_t> DecodeBase64(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    int quartet[4] = { 0, 0, 0, 0 };
    int quartet_size = 0;

    for (char c : text) {
        const int value = Base64Value(c);
        if (value == -1) continue;
        quartet[quartet_size++] = value;
        if (quartet_size != 4) continue;

        assert(quartet[0] >= 0 && quartet[1] >= 0);
        bytes.push_back(static_cast<std::uint8_t>((quartet[0] << 2) | (quartet[1] >> 4)));
        if (quartet[2] != -2) {
            bytes.push_back(
                static_cast<std::uint8_t>(((quartet[1] & 0x0f) << 4) | (quartet[2] >> 2)));
        }
        if (quartet[3] != -2) {
            bytes.push_back(
                static_cast<std::uint8_t>(((quartet[2] & 0x03) << 6) | quartet[3]));
        }
        quartet_size = 0;
    }

    return bytes;
}

struct WorkshopFixture {
    std::filesystem::path dir;
    std::filesystem::path clip_path;

    WorkshopFixture() {
        dir = std::filesystem::temp_directory_path()
            / ("we-renderer-video-pipeline-test-" + std::to_string(::getpid()));
        clip_path = dir / "clip.mp4";
        std::filesystem::create_directories(dir);

        {
            std::ofstream project(dir / "project.json");
            project << kProjectJson;
        }

        const auto clip_bytes = DecodeBase64(kFixtureMp4Base64);
        {
            std::ofstream clip(clip_path, std::ios::binary);
            clip.write(reinterpret_cast<const char*>(clip_bytes.data()),
                       static_cast<std::streamsize>(clip_bytes.size()));
        }
    }

    ~WorkshopFixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

void SkipTest(const char* reason) {
    std::fprintf(stderr, "video-pipeline-test: skip: %s\n", reason);
    std::exit(0);
}

bool DecoderAdvertisesDmabufDrmFormat() {
    FILE* pipe = ::popen("gst-inspect-1.0 vah264dec 2>/dev/null", "r");
    if (pipe == nullptr) return false;

    std::ostringstream output;
    char buffer[512];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output << buffer;
    }
    const int status = ::pclose(pipe);
    if (status != 0) return false;

    const std::string text = output.str();
    return text.find("memory:DMABuf") != std::string::npos &&
           text.find("drm-format") != std::string::npos;
}

we_frame_v1 WaitForFrame(we_session_t* session, we_frame_kind_v1 expected_kind) {
    using namespace std::chrono_literals;

    for (int attempt = 0; attempt < 200; ++attempt) {
        assert(we_session_tick(session) == 0);

        we_frame_v1 frame {};
        frame.size = sizeof(frame);
        const int acquire_result = we_session_acquire_frame(session, &frame);
        if (acquire_result == 0) {
            if (frame.kind != expected_kind) {
                std::fprintf(stderr,
                             "video-pipeline-test: expected frame kind %u but got %u\n",
                             static_cast<unsigned>(expected_kind),
                             static_cast<unsigned>(frame.kind));
                assert(false);
            }
            assert(frame.width > 0);
            assert(frame.height > 0);
            return frame;
        }
        assert(acquire_result == 1);
        std::this_thread::sleep_for(10ms);
    }

    assert(false && "timed out waiting for decoded frame");
    return {};
}

void RunShmPipelineTest(const WorkshopFixture& fixture) {
    we_session_t* session = we_session_create_with_cache_path("/tmp/we-renderer-video-shm-test-cache");
    assert(session != nullptr);

    we_source_v1 source {};
    source.size = static_cast<std::uint32_t>(sizeof(source));
    source.version = 1;
    source.uri = fixture.dir.c_str();
    assert(we_session_set_source(session, &source) == 0);

    we_render_config_v1 config {};
    config.size = sizeof(config);
    config.version = 1;
    config.width = 16;
    config.height = 16;
    config.prefer_dmabuf = false;
    config.allow_shm_fallback = true;
    assert(we_session_set_render_config(session, &config) == 0);
    assert(we_session_play(session) == 0);

    we_frame_v1 frame = WaitForFrame(session, WE_FRAME_KIND_SHM);
    assert(frame.shm_stride > 0);
    assert(frame.shm_size > 0);
    assert(frame.planes[0].fd >= 0);
    we_frame_release(&frame);

    assert(we_session_stop(session) == 0);
    we_session_destroy(session);
}

void RunDmabufPipelineTest(const WorkshopFixture& fixture) {
    if (! DecoderAdvertisesDmabufDrmFormat()) {
        SkipTest("vah264dec with DMA-BUF drm-format caps is unavailable");
    }

    we_session_t* session = we_session_create_with_cache_path("/tmp/we-renderer-video-dmabuf-test-cache");
    assert(session != nullptr);

    we_source_v1 source {};
    source.size = static_cast<std::uint32_t>(sizeof(source));
    source.version = 1;
    source.uri = fixture.dir.c_str();
    assert(we_session_set_source(session, &source) == 0);

    we_render_config_v1 config {};
    config.size = sizeof(config);
    config.version = 1;
    config.width = 16;
    config.height = 16;
    config.prefer_dmabuf = true;
    config.allow_shm_fallback = false;
    assert(we_session_set_render_config(session, &config) == 0);
    assert(we_session_play(session) == 0);

    we_frame_v1 frame = WaitForFrame(session, WE_FRAME_KIND_DMABUF);
    assert(frame.n_planes > 0);
    assert(frame.drm_fourcc != 0);
    for (std::uint32_t i = 0; i < frame.n_planes && i < 4; ++i) {
        assert(frame.planes[i].fd >= 0);
        assert(frame.planes[i].stride > 0);
    }
    we_frame_release(&frame);

    assert(we_session_stop(session) == 0);
    we_session_destroy(session);
}
} // namespace

int main() {
    WorkshopFixture fixture;
    RunShmPipelineTest(fixture);
    RunDmabufPipelineTest(fixture);
    return 0;
}
