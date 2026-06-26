#pragma once

// Self-contained CMock-style fake for the web_backend_contract_test.
// Records every WebBrowserHost call in member vectors; tests then
// assert the recorded sequence matches the contract. No external
// CMock / FakeIt / gMock dependency — the surface is small enough
// that a hand-rolled recorder is more readable than pulling a
// mocking framework into the build.
//
// Subclasses virtual WebBrowserHost (made polymorphic in commit
// 11) and overrides every entry point. The test creates one,
// injects it into a WebBackend via testSetBrowserHost, and runs
// the full session lifecycle. After Shutdown, the recorded calls
// should match the expected sequence for the operations the test
// performed.

#include "wallpaper/web/WebBrowserHost.hpp"
#include "wallpaper/web/WebTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wallpaper::test
{
struct RecordedCall {
    std::string name;
};

class MockWebBrowserHost : public WebBrowserHost {
public:
    MockWebBrowserHost() = default;
    ~MockWebBrowserHost() override = default;

    // Recorded values
    std::vector<RecordedCall>             calls;
    WebBrowserHost::InitOptions           last_init_opts {};
    WebManifestData                       last_manifest;
    std::filesystem::path                 last_workshop_dir;
    int                                   last_open_width { 0 };
    int                                   last_open_height { 0 };
    int                                   resize_w { 0 };
    int                                   resize_h { 0 };
    int                                   mouse_move_x { 0 };
    int                                   mouse_move_y { 0 };
    int                                   mouse_button_x { 0 };
    int                                   mouse_button_y { 0 };
    int                                   mouse_button_cef { 0 };
    bool                                  mouse_button_down { false };
    int                                   mouse_button_clicks { 0 };
    float                                 last_volume { 0.0f };
    int                                   last_fps { 0 };
    bool                                  last_paused { false };
    int                                   focus_gained_count { 0 };
    int                                   pump_count { 0 };
    int                                   shutdown_count { 0 };
    int                                   push_audio_count { 0 };
    bool                                  has_accelerated_paint_callback { false };
    AcceleratedPaintCallback              accelerated_paint_callback;
    std::vector<float>                    last_audio;
    std::string                           last_user_key;
    std::string                           last_user_value_json;

    bool Init(const InitOptions& opts) override {
        calls.push_back({"Init"});
        last_init_opts = opts;
        return true;
    }

    void SetAcceleratedPaintCallback(AcceleratedPaintCallback cb) override {
        calls.push_back({"SetAcceleratedPaintCallback"});
        has_accelerated_paint_callback = static_cast<bool>(cb);
        accelerated_paint_callback = std::move(cb);
        WebBrowserHost::SetAcceleratedPaintCallback(accelerated_paint_callback);
    }

    bool OpenWallpaper(const WebManifestData& manifest,
                       const std::filesystem::path& workshop_dir,
                       int width, int height) override {
        calls.push_back({"OpenWallpaper"});
        last_manifest     = manifest;
        last_workshop_dir = workshop_dir;
        last_open_width   = width;
        last_open_height  = height;
        return true;
    }

    void OnResize(int width, int height) override {
        calls.push_back({"OnResize"});
        resize_w = width;
        resize_h = height;
    }

    void Invalidate() override { calls.push_back({"Invalidate"}); }

    void OnMouseMove(int x, int y, bool /*left_down*/) override {
        calls.push_back({"OnMouseMove"});
        mouse_move_x = x;
        mouse_move_y = y;
    }

    void OnMouseButton(int x, int y, int cef_button, bool down, int click_count) override {
        calls.push_back({"OnMouseButton"});
        mouse_button_x       = x;
        mouse_button_y       = y;
        mouse_button_cef     = cef_button;
        mouse_button_down    = down;
        mouse_button_clicks  = click_count;
    }

    void OnMouseWheel(int /*x*/, int /*y*/, int /*dx*/, int /*dy*/) override {
        calls.push_back({"OnMouseWheel"});
    }

    void OnKey(int /*type*/, int /*native*/, int /*vk*/, int /*mod*/, unsigned int /*u*/) override {
        calls.push_back({"OnKey"});
    }

    void OnFocus(bool gained) override {
        calls.push_back({"OnFocus"});
        if (gained) ++focus_gained_count;
    }

    void Pump() override { calls.push_back({"Pump"}); ++pump_count; }

    void ApplyVolume(float volume) override {
        calls.push_back({"ApplyVolume"});
        last_volume = volume;
    }

    void SetFrameRate(int fps) override {
        calls.push_back({"SetFrameRate"});
        last_fps = fps;
    }

    void SetPaused(bool paused) override {
        calls.push_back({"SetPaused"});
        last_paused = paused;
    }

    void ApplyUserProperty(std::string_view key, std::string_view value_json) override {
        calls.push_back({"ApplyUserProperty"});
        last_user_key        = std::string(key);
        last_user_value_json = std::string(value_json);
    }

    void PushAudioData(const float* data, std::size_t count) override {
        calls.push_back({"PushAudioData"});
        ++push_audio_count;
        last_audio.assign(data, data + count);
    }

    bool ShouldExit() const override { return false; }

    void RequestClose() override { calls.push_back({"RequestClose"}); }

    void Shutdown() override {
        calls.push_back({"Shutdown"});
        ++shutdown_count;
    }

    // Test helpers
    bool hasCall(const std::string& name) const {
        for (const auto& c : calls) if (c.name == name) return true;
        return false;
    }

    std::size_t callCount(const std::string& name) const {
        std::size_t n = 0;
        for (const auto& c : calls) if (c.name == name) ++n;
        return n;
    }
};
} // namespace wallpaper::test
