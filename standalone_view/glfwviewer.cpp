#include <iostream>
#include <set>
#include <fstream>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <atomic>
#include "arg.hpp"
#include "wallpaper/WallpaperRuntime.hpp"
#include "wallpaper/scene/WEScene.hpp"

using namespace std;

atomic<bool> renderCall(false);

struct UserData {
    wallpaper::WallpaperSession* session { nullptr };

    uint16_t width;
    uint16_t height;
};

namespace
{
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
}

void updateCallback() {
    renderCall = true;
    glfwPostEmptyEvent();
}

int main(int argc, char** argv) {
    argparse::ArgumentParser program("scene-viewer");
    setAndParseArg(program, argc, argv);
    auto [w_width, w_height] = program.get<Resolution>(OPT_RESOLUTION);

    glfwSetErrorCallback(glfw_error_callback);
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(w_width, w_height, "WP", nullptr, nullptr);
    if (window == nullptr) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
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

    auto& sf_info = info.surface_info;
    {
        uint32_t glfwExtCount = 0;
        auto     exts         = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        for (int i = 0; i < glfwExtCount; i++) {
            sf_info.instanceExts.emplace_back(exts[i]);
        }

        sf_info.createSurfaceOp = [window](VkInstance inst, VkSurfaceKHR* surface) {
            return glfwCreateWindowSurface(inst, window, NULL, surface);
        };
    }

    glfwShowWindow(window);

    wallpaper::WallpaperRuntime runtime;
    std::string cache_path = program.get<std::string>(OPT_CACHE_PATH);

    auto session = wallpaper::CreateWESceneSession(runtime, cache_path);
    data.session = session.get();

    wallpaper::WESceneSourceConfig sourceConfig;
    sourceConfig.uri      = program.get<std::string>(ARG_SCENE);
    sourceConfig.assets   = program.get<std::string>(ARG_ASSETS);
    sourceConfig.graphviz = program.get<bool>(OPT_GRAPHVIZ);
    sourceConfig.fps      = program.get<int32_t>(OPT_FPS);

    if (auto result = wallpaper::LoadWEScene(*session, sourceConfig); ! result) {
        std::cerr << "LoadWEScene failed: " << result.error().message << std::endl;
        return -1;
    }
    if (auto result = wallpaper::BindWESceneOutput(*session, info); ! result) {
        std::cerr << "BindWESceneOutput failed: " << result.error().message << std::endl;
        return -1;
    }
    if (auto result = session->play(); ! result) {
        std::cerr << "session->play failed: " << result.error().message << std::endl;
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
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
