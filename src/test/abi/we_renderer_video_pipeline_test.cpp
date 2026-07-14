#include "wallpaper/abi/WeRenderer.h"
#include "common/result/Result.hpp"

#include <drm/drm_fourcc.h>

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
constexpr std::string_view kFixtureMp4Base64 =
    "AAAAFGZ0eXBxdCAgIAUDAHF0ICAAAAAIZnJlZQAAAwdtZGF0AAAAAgkQAAAAG2dCwAraEJsBagwMDUoAAAMAAgAAAwAJHi"
    "RNQAAAAARozjyAAAACogYF//"
    "+e3EXpvebZSLeWLNgg2SPu73gyNjQgLSBjb3JlIDE2NSByMzIyMiBiMzU2MDVhIC0gSC4yNjQvTVBFRy00IEFWQyBjb2Rl"
    "YyAtIENvcHlsZWZ0IDIwMDMtMjAyNSAtIGh0dHA6Ly93d3cudmlkZW9sYW4ub3JnL3gyNjQuaHRtbCAtIG9wdGlvbnM6IG"
    "NhYmFjPTAgcmVmPTEgZGVibG9jaz0wOjA6MCBhbmFseXNlPTA6MCBtZT1kaWEgc3VibWU9MCBwc3k9MSBwc3lfcmQ9MS4w"
    "MDowLjAwIG1peGVkX3JlZj0wIG1lX3JhbmdlPTE2IGNocm9tYV9tZT0xIHRyZWxsaXM9MCA4eDhkY3Q9MCBjcW09MCBkZW"
    "Fkem9uZT0yMSwxMSBmYXN0X3Bza2lwPTEgY2hyb21hX3FwX29mZnNldD0wIHRocmVhZHM9MSBsb29rYWhlYWRfdGhyZWFk"
    "cz0xIHNsaWNlZF90aHJlYWRzPTAgbnI9MCBkZWNpbWF0ZT0xIGludGVybGFjZWQ9MCBibHVyYXlfY29tcGF0PTAgY29uc3"
    "RyYWluZWRfaW50cmE9MCBiZnJhbWVzPTAgd2VpZ2h0cD0wIGtleWludD0yIGtleWludF9taW49MSBzY2VuZWN1dD0wIGlu"
    "dHJhX3JlZnJlc2g9MCByY19sb29rYWhlYWQ9MCByYz1jYnIgbWJ0cmVlPTAgYml0cmF0ZT0zMiByYXRldG9sPTEuMCBxY2"
    "9tcD0wLjYwIHFwbWluPTAgcXBtYXg9NjkgcXBzdGVwPTQgdmJ2X21heHJhdGU9MzIgdmJ2X2J1ZnNpemU9MTkgbmFsX2hy"
    "ZD1ub25lIGZpbGxlcj0wIGlwX3JhdGlvPTEuNDAgYXE9MACAAAAAGGWIhAaokwoAA/"
    "vyTycnXXXXXXXXXXXXgAAAAAIJMAAAAAZBmiAaoIwAAAOkbW9vdgAAAGxtdmhkAAAAAOZ7THXme0x1AAAMgAAADIAAAQAA"
    "AQAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAgAAAvN0cmFrAAAAXHRraGQAAAAH5ntMdeZ7THUAAAABAAAAAAAADIAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
    "AAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAEAAAABAAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAAAAEAAAyAAAAAAAABAA"
    "AAAAISbWRpYQAAACBtZGhkAAAAAOZ7THXme0x1AAAAyAAAAMhVxAAAAAAALWhkbHIAAAAAbWhscnZpZGUAAAAAAAAAAAAA"
    "AAAMVmlkZW9IYW5kbGVyAAABvW1pbmYAAAAUdm1oZAAAAAEAQIAAgACAAAAAACFoZGxyAAAAAGRobHJhbGlzAAAAAAAAAA"
    "AAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAxhbGlzAAAAAQAAAVxzdGJsAAAA3HN0c2QAAAAAAAAAAQAAAMxh"
    "dmMxAAAAAAAAAAEAAAAAAAAAAAAAAgAAAAIAAEAAQABIAAAASAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAGP//AAAAMmF2Y0MBQsAK/"
    "+EAG2dCwAraEJsBagwMDUoAAAMAAgAAAwAJHiRNQAEABGjOPIAAAAAUYnRydAAAAAAAAIAAAAAX+"
    "AAAABJjb2xybmNsYwAGAAYABgAAAApmaWVsAQAAAAAQcGFzcAAAAAEAAAABAAAAAAAAABhzdHRzAAAAAAAAAAEAAAACAAA"
    "AZAAAABRzdHNzAAAAAAAAAAEAAAABAAAAHHN0c2MAAAAAAAAAAQAAAAEAAAACAAAAAQAAABxzdHN6AAAAAAAAAAAAAAACA"
    "AAC7wAAABAAAAAUc3RjbwAAAAAAAAABAAAAJAAAAFl1ZHRhAAAAUW1ldGEAAAAAAAAAIWhkbHIAAAAAbWhscm1kaXIAAAA"
    "AAAAAAAAAAAAAAAAAJGlsc3QAAAAcqXRvbwAAABRkYXRhAAAAAQAAAAB4MjY0AAAAPXVkdGEAAAA1bWV0YQAAAAAAAAAha"
    "GRscgAAAABtaGxybWRpcgAAAAAAAAAAAAAAAAAAAAAIaWxzdA==";

constexpr std::string_view kFixtureWebmBase64 =
    "GkXfowEAAAAAAAAQQoKFd2VibQBCh4EEQoWBAhhTgGcBAAAAAAACIxFNm3QBAAAAAAAAjE27AQAAAAAAABJTq4QVSalmU6"
    "yIAAAAAAAAAJhNuwEAAAAAAAASU6uEFlSua1OsiAAAAAAAAAEF7JoBAAAAAAAAElOrhBBDp3BTrIj//////////"
    "027AQAAAAAAABJTq4QcU7trU6yIAAAAAAAAAfvsmgEAAAAAAAASU6uEElTDZ1OsiP//////////"
    "FUmpZgEAAAAAAABhKtexgw9CQESJiECPQAAAAAAATYClR1N0cmVhbWVyIG1hdHJvc2thbXV4IHZlcnNpb24gMS4yOC41AF"
    "dBmUdTdHJlYW1lciBNYXRyb3NrYSBtdXhlcgBEYYgLLmESh6LWmBZUrmsBAAAAAAAAZ64BAAAAAAAAXteBAYOBAXPFiC/"
    "NXIxBm1NRI+"
    "ODhA7msoBTboZWaWRlbwDgAQAAAAAAACuwgUC6gUCagQJVsAEAAAAAAAAYVbmBAVWxgQZVuoEGVbuBBlW3gQJVuIEChoZW"
    "X1ZQOAAfQ7Z1AQAAAAAAAHfngQCjqoEAAIDwAgCdASpAAEAAAEcIhYWImYSIAgIABnA8QmAKsiD3MAD+/"
    "6tQgKOWgQD6ANEBAAEQEAAYABhYL/QACI6AAKOWgQH0ANEBAAEQEAAYABhYL/QACI6AAKOWgQLuANEBAAEQEAAYABhYL/"
    "QACI6AABxTu2sBAAAAAAAAHLsBAAAAAAAAE7OBALcBAAAAAAAAB/eBAfGCAXg=";

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
    int                       quartet[4]   = { 0, 0, 0, 0 };
    int                       quartet_size = 0;

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
            bytes.push_back(static_cast<std::uint8_t>(((quartet[2] & 0x03) << 6) | quartet[3]));
        }
        quartet_size = 0;
    }

    return bytes;
}

struct WorkshopFixture {
    std::filesystem::path dir;
    std::filesystem::path clip_path;

    WorkshopFixture(std::string_view name, std::string_view filename,
                    std::string_view fixture_base64) {
        dir       = std::filesystem::temp_directory_path() /
                    ("we-renderer-video-pipeline-test-" + std::string(name) + "-" +
                     std::to_string(::getpid()));
        clip_path = dir / filename;
        std::filesystem::create_directories(dir);

        {
            std::ofstream project(dir / "project.json");
            project << R"({"type":"video","file":")" << filename
                    << R"(","title":"Video Pipeline Test"})";
        }

        const auto clip_bytes = DecodeBase64(fixture_base64);
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

bool PostprocAdvertisesCompatibleDmabufFormat() {
    FILE* pipe = ::popen("gst-inspect-1.0 vapostproc 2>/dev/null", "r");
    if (pipe == nullptr) return false;

    std::ostringstream output;
    char               buffer[512];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output << buffer;
    }
    const int status = ::pclose(pipe);
    if (status != 0) return false;

    const std::string text = output.str();
    return text.find("memory:DMABuf") != std::string::npos &&
           (text.find("AB24") != std::string::npos || text.find("XB24") != std::string::npos ||
            text.find("AR24") != std::string::npos || text.find("XR24") != std::string::npos);
}

void RequireSessionSuccess(we_session_t* session, int result, const char* operation) {
    if (result == 0) return;

    std::uint32_t size = 0;
    std::string   diagnostics;
    if (we_session_get_diagnostics_json(session, nullptr, &size) == 0 && size > 0) {
        diagnostics.resize(size);
        if (we_session_get_diagnostics_json(session, diagnostics.data(), &size) != 0) {
            diagnostics = "<failed to read diagnostics>";
        }
    }
    std::fprintf(stderr,
                 "video-pipeline-test: %s failed with %d: %s\n",
                 operation,
                 result,
                 diagnostics.empty() ? "<no diagnostics>" : diagnostics.c_str());
    assert(false);
}

we_frame_v1 WaitForFrame(we_session_t* session, we_frame_kind_v1 expected_kind) {
    using namespace std::chrono_literals;

    for (int attempt = 0; attempt < 200; ++attempt) {
        assert(we_session_tick(session) == 0);

        we_frame_v1 frame {};
        frame.size               = sizeof(frame);
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
    we_session_t* session =
        we_session_create_with_cache_path("/tmp/we-renderer-video-shm-test-cache");
    assert(session != nullptr);

    we_source_v1 source {};
    source.size    = static_cast<std::uint32_t>(sizeof(source));
    source.version = 1;
    source.uri     = fixture.dir.c_str();
    RequireSessionSuccess(session, we_session_set_source(session, &source), "set SHM source");

    we_render_config_v1 config {};
    config.size               = sizeof(config);
    config.version            = 1;
    config.width              = 16;
    config.height             = 16;
    config.prefer_dmabuf      = false;
    config.allow_shm_fallback = true;
    RequireSessionSuccess(
        session, we_session_set_render_config(session, &config), "set SHM render config");
    RequireSessionSuccess(session, we_session_play(session), "play SHM video");

    we_frame_v1 frame = WaitForFrame(session, WE_FRAME_KIND_SHM);
    assert(frame.width == config.width);
    assert(frame.height == config.height);
    assert(frame.shm_stride > 0);
    assert(frame.shm_size > 0);
    assert(frame.planes[0].fd >= 0);
    we_frame_release(&frame);

    RequireSessionSuccess(session, we_session_stop(session), "stop SHM video");
    RequireSessionSuccess(session, we_session_play(session), "restart SHM video");
    frame = WaitForFrame(session, WE_FRAME_KIND_SHM);
    assert(frame.width == config.width);
    assert(frame.height == config.height);
    we_frame_release(&frame);
    RequireSessionSuccess(session, we_session_stop(session), "stop restarted SHM video");
    we_session_destroy(session);
}

void RunDmabufPipelineTest(const WorkshopFixture& fixture) {
    if (! PostprocAdvertisesCompatibleDmabufFormat()) {
        std::fprintf(
            stderr, "video-pipeline-test: skip DMA-BUF: vapostproc has no compatible RGB format\n");
        return;
    }

    we_session_t* session =
        we_session_create_with_cache_path("/tmp/we-renderer-video-dmabuf-test-cache");
    assert(session != nullptr);

    we_source_v1 source {};
    source.size    = static_cast<std::uint32_t>(sizeof(source));
    source.version = 1;
    source.uri     = fixture.dir.c_str();
    RequireSessionSuccess(session, we_session_set_source(session, &source), "set DMA-BUF source");

    we_render_config_v1 config {};
    config.size               = sizeof(config);
    config.version            = 1;
    config.width              = 16;
    config.height             = 16;
    config.prefer_dmabuf      = true;
    config.allow_shm_fallback = false;
    RequireSessionSuccess(
        session, we_session_set_render_config(session, &config), "set DMA-BUF render config");
    RequireSessionSuccess(session, we_session_play(session), "play DMA-BUF video");

    we_frame_v1 frame = WaitForFrame(session, WE_FRAME_KIND_DMABUF);
    assert(frame.width == config.width);
    assert(frame.height == config.height);
    assert(frame.n_planes > 0);
    assert(frame.drm_fourcc != 0);
    for (std::uint32_t i = 0; i < frame.n_planes && i < 4; ++i) {
        assert(frame.planes[i].fd >= 0);
        assert(frame.planes[i].stride > 0);
    }
    const std::uint32_t selectedFourcc   = frame.drm_fourcc;
    const std::uint64_t selectedModifier = frame.drm_modifier;
    we_frame_release(&frame);

    RequireSessionSuccess(
        session,
        we_session_set_dmabuf_formats(session, &selectedFourcc, &selectedModifier, 1),
        "apply exact DMA-BUF consumer format while playing");
    frame = WaitForFrame(session, WE_FRAME_KIND_DMABUF);
    assert(frame.width == config.width);
    assert(frame.height == config.height);
    assert(frame.drm_fourcc == selectedFourcc);
    assert(frame.drm_modifier == selectedModifier);
    we_frame_release(&frame);

    constexpr std::uint32_t resizedWidth  = 24;
    constexpr std::uint32_t resizedHeight = 12;
    RequireSessionSuccess(session,
                          we_session_resize_output(session, resizedWidth, resizedHeight),
                          "resize DMA-BUF output while playing");
    frame = WaitForFrame(session, WE_FRAME_KIND_DMABUF);
    assert(frame.width == resizedWidth);
    assert(frame.height == resizedHeight);
    assert(frame.drm_fourcc == selectedFourcc);
    assert(frame.drm_modifier == selectedModifier);
    we_frame_release(&frame);

    const std::uint32_t incompatibleFourcc = DRM_FORMAT_NV12;
    const std::int32_t  incompatibleResult =
        we_session_set_dmabuf_formats(session, &incompatibleFourcc, &selectedModifier, 1);
    assert(incompatibleResult ==
           static_cast<std::int32_t>(wallpaper::ResultCode::NotSupported) + 1);

    frame = WaitForFrame(session, WE_FRAME_KIND_DMABUF);
    assert(frame.width == resizedWidth);
    assert(frame.height == resizedHeight);
    assert(frame.drm_fourcc == selectedFourcc);
    assert(frame.drm_modifier == selectedModifier);
    we_frame_release(&frame);

    assert(we_session_stop(session) == 0);
    we_session_destroy(session);
}
} // namespace

int main() {
    WorkshopFixture mp4_fixture("mp4", "clip.mp4", kFixtureMp4Base64);
    WorkshopFixture webm_fixture("webm", "clip.webm", kFixtureWebmBase64);
    RunShmPipelineTest(mp4_fixture);
    RunShmPipelineTest(webm_fixture);
    RunDmabufPipelineTest(mp4_fixture);
    return 0;
}
