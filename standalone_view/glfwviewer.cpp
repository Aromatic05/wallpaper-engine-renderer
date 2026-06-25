#include <iostream>
#include <set>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cmath>
#include <vector>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <atomic>
#include "arg.hpp"
#include "wallpaper/WallpaperRuntime.hpp"
#include "wallpaper/scene/WEScene.hpp"
#include "wallpaper/scene/WESceneContract.hpp"

using namespace std;

atomic<bool> renderCall(false);

struct UserData {
    wallpaper::WallpaperSession* session { nullptr };

    uint16_t width;
    uint16_t height;
};

namespace
{
constexpr float kPi = 3.14159265358979323846f;

void glfw_error_callback(int code, const char* description) {
    std::cerr << "GLFW error [" << code << "]: "
              << (description == nullptr ? "unknown" : description) << std::endl;
}

wallpaper::InputEvent makePointerEvent(const UserData& data,
                                       wallpaper::InputEventType type,
                                       double xpos,
                                       double ypos) {
    wallpaper::InputEvent event;
    event.type     = type;
    event.pointerX = xpos / data.width;
    event.pointerY = ypos / data.height;
    return event;
}
}

extern "C" {
void framebuffer_size_callback(GLFWwindow*, int width, int height) {}

void mouse_button_callback(GLFWwindow* win, int button, int action, int mods) {
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    if (action != GLFW_PRESS && action != GLFW_RELEASE) return;

    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (data == nullptr || data->session == nullptr) return;

    double xpos { 0.0 };
    double ypos { 0.0 };
    glfwGetCursorPos(win, &xpos, &ypos);

    auto event = makePointerEvent(*data,
                                  action == GLFW_PRESS ? wallpaper::InputEventType::PointerDown
                                                       : wallpaper::InputEventType::PointerUp,
                                  xpos,
                                  ypos);
    data->session->sendInput(event);
}

void cursor_position_callback(GLFWwindow* win, double xpos, double ypos) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (data == nullptr || data->session == nullptr) return;

    auto event = makePointerEvent(*data, wallpaper::InputEventType::PointerMove, xpos, ypos);
    data->session->sendInput(event);
}

std::shared_ptr<std::vector<float>> makeAudioPatternSamples(const std::string& pattern_name) {
    constexpr size_t kChannelSize = 64;

    auto samples = std::make_shared<std::vector<float>>();
    if (pattern_name == "off") return samples;

    samples->assign(kChannelSize * 2, 0.0f);
    auto* left  = samples->data();
    auto* right = samples->data() + kChannelSize;

    if (pattern_name == "bars") {
        for (size_t i = 0; i < kChannelSize; i++) {
            const float normalized = 1.0f - static_cast<float>(i) / static_cast<float>(kChannelSize - 1);
            left[i]  = std::max(0.0f, normalized);
            right[i] = std::max(0.0f, normalized * 0.85f);
        }
        return samples;
    }

    if (pattern_name == "sweep") {
        for (size_t i = 0; i < kChannelSize; i++) {
            const float position = static_cast<float>(i) / static_cast<float>(kChannelSize - 1);
            left[i]  = std::exp(-48.0f * std::pow(position - 0.25f, 2.0f));
            right[i] = std::exp(-48.0f * std::pow(position - 0.72f, 2.0f));
        }
        return samples;
    }

    if (pattern_name == "pulse") {
        for (size_t i = 0; i < kChannelSize; i++) {
            const float phase = static_cast<float>(i) / static_cast<float>(kChannelSize - 1);
            const float wave  = 0.5f + 0.5f * std::sin(phase * 6.0f * kPi);
            left[i]           = 0.15f + 0.85f * wave;
            right[i]          = 0.15f + 0.85f * (1.0f - wave);
        }
        return samples;
    }

    return nullptr;
}
}

void updateCallback() {
    renderCall = true;
    glfwPostEmptyEvent();
}

int main(int argc, char** argv) {
    argparse::ArgumentParser program("scene-viewer");
    setAndParseArg(program, argc, argv);
    auto [w_width, w_height] = program.get<Resolution>(OPT_RESOLUTION);
    const std::string dump_frame_path = program.get<std::string>(OPT_DUMP_FRAME);
    const bool dump_frame = !dump_frame_path.empty();
    const int32_t dump_frame_number = std::max<int32_t>(1, program.get<int32_t>(OPT_DUMP_FRAME_NUMBER));
    const std::string audio_pattern = program.get<std::string>(OPT_AUDIO_PATTERN);

    glfwSetErrorCallback(glfw_error_callback);
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = nullptr;
    if (!dump_frame) {
        window = glfwCreateWindow(w_width, w_height, "WP", nullptr, nullptr);
        if (window == nullptr) {
            std::cout << "Failed to create GLFW window" << std::endl;
            glfwTerminate();
            return -1;
        }
    }

    UserData data;
    data.width  = w_width;
    data.height = w_height;

    wallpaper::RenderInitInfo info;
    info.enable_valid_layer = program.get<bool>(OPT_VALID_LAYER);
    info.width              = static_cast<uint16_t>(w_width);
    info.height             = static_cast<uint16_t>(w_height);
    info.render_scale       = 1.0;
    info.redraw_callback    = updateCallback;
    info.offscreen          = dump_frame;

    auto& sf_info = info.surface_info;
    if (!dump_frame) {
        uint32_t glfwExtCount = 0;
        auto     exts         = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        for (int i = 0; i < glfwExtCount; i++) {
            sf_info.instanceExts.emplace_back(exts[i]);
        }

        sf_info.createSurfaceOp = [window](VkInstance inst, VkSurfaceKHR* surface) {
            return glfwCreateWindowSurface(inst, window, NULL, surface);
        };
    }

    if (!dump_frame) {
        glfwShowWindow(window);
    }

    wallpaper::WallpaperRuntime runtime;
    std::string cache_path = program.get<std::string>(OPT_CACHE_PATH);

    auto session = wallpaper::CreateWESceneSession(runtime, cache_path);
    data.session = session.get();

    wallpaper::WESceneSourceConfig sourceConfig;
    sourceConfig.uri      = program.get<std::string>(ARG_SCENE);
    sourceConfig.assets   = program.get<std::string>(ARG_ASSETS);
    sourceConfig.graphviz = program.get<bool>(OPT_GRAPHVIZ);
    sourceConfig.fps      = program.get<int32_t>(OPT_FPS);
    const int32_t capture_fps = std::max<int32_t>(1, sourceConfig.fps);

    if (auto result = wallpaper::LoadWEScene(*session, sourceConfig); ! result) {
        std::cerr << "LoadWEScene failed: " << result.error().message << std::endl;
        return -1;
    }
    auto binding_result = wallpaper::BindWESceneOutput(*session, info);
    if (!binding_result) {
        std::cerr << "BindWESceneOutput failed: " << binding_result.error().message << std::endl;
        return -1;
    }
    if (auto result = session->play(); ! result) {
        std::cerr << "session->play failed: " << result.error().message << std::endl;
        return -1;
    }

    if (auto audio_samples = makeAudioPatternSamples(audio_pattern); audio_samples == nullptr) {
        std::cerr << "unknown audio pattern: " << audio_pattern << std::endl;
        return -1;
    } else if (!audio_samples->empty()) {
        if (auto result = session->setProperty(wallpaper::WE_SCENE_PROPERTY_AUDIO_SAMPLES,
                                               audio_samples);
            !result) {
            std::cerr << "failed to set audio samples: " << result.error().message << std::endl;
            return -1;
        }
    }

    if (dump_frame) {
        const auto dump_parent = std::filesystem::path(dump_frame_path).parent_path();
        if (!dump_parent.empty()) {
            std::filesystem::create_directories(dump_parent);
        }
        std::error_code remove_error;
        std::filesystem::remove(dump_frame_path, remove_error);
        if (auto result = session->setProperty(wallpaper::WE_SCENE_PROPERTY_CAPTURE_FRAME_NUMBER,
                                               dump_frame_number);
            !result) {
            std::cerr << "failed to set capture frame number: " << result.error().message
                      << std::endl;
            return -1;
        }
        if (auto result = session->setProperty(wallpaper::WE_SCENE_PROPERTY_CAPTURE_FRAME,
                                               dump_frame_path);
            !result) {
            std::cerr << "capture request failed: " << result.error().message << std::endl;
            return -1;
        }

        const auto expected_capture_time =
            std::chrono::milliseconds((1000ll * dump_frame_number) / capture_fps);
        const auto capture_timeout =
            std::max(std::chrono::milliseconds(20000),
                     std::chrono::milliseconds(12000) + expected_capture_time * 2);
        // Offscreen capture waits for scene load plus the requested rendered frame count. A fixed
        // 20s budget is too tight for heavier workshop scenes when validating later frames such as
        // frame 120, so scale the wait window with the authored fps while still keeping a healthy
        // startup slack for asset upload and script warmup.
        const auto deadline = std::chrono::steady_clock::now() + capture_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto tickResult = runtime.tick(*session); !tickResult) {
                std::cerr << "runtime.tick failed: " << tickResult.error().message << std::endl;
                return -1;
            }
            if (std::filesystem::exists(dump_frame_path)) {
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        std::cerr << "timed out waiting for frame dump: " << dump_frame_path << std::endl;
        return -1;
    }

    glfwSetWindowUserPointer(window, &data);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);

    while (! glfwWindowShouldClose(window)) {
        if (auto tickResult = runtime.tick(*session); ! tickResult) {
            std::cerr << "runtime.tick failed: " << tickResult.error().message << std::endl;
            break;
        }
        glfwPollEvents();
    }
    // wgl.Clear();
    if (window != nullptr) {
        glfwDestroyWindow(window);
    }
    glfwTerminate();
    return 0;
}
