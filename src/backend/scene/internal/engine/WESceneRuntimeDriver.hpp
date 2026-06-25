#pragma once

// Internal driver that isolates the older scene/render pipeline behind the scene backend interface.
// New integrations should prefer WallpaperSession plus api/scene/WEScene.hpp helpers.

#include <memory>
#include <string_view>
#include <functional>
#include "common/scene/WESceneContract.hpp"
#include "host/HostServices.hpp"
#include "Type.hpp"
#include "output/swapchain/ExSwapchain.hpp"

#include "../../../../../include/wallpaper/scene/WESceneEngineServices.hpp"

namespace wallpaper
{
class WESceneOutputBinding;

constexpr std::string_view PROPERTY_SOURCE               = WE_SCENE_PROPERTY_SOURCE;
constexpr std::string_view PROPERTY_ASSETS               = WE_SCENE_PROPERTY_ASSETS;
constexpr std::string_view PROPERTY_FPS                  = WE_SCENE_PROPERTY_FPS;
constexpr std::string_view PROPERTY_FILLMODE             = WE_SCENE_PROPERTY_FILLMODE;
constexpr std::string_view PROPERTY_SPEED                = WE_SCENE_PROPERTY_SPEED;
constexpr std::string_view PROPERTY_GRAPHIVZ             = WE_SCENE_PROPERTY_GRAPHIVZ;
constexpr std::string_view PROPERTY_VOLUME               = WE_SCENE_PROPERTY_VOLUME;
constexpr std::string_view PROPERTY_MUTED                = WE_SCENE_PROPERTY_MUTED;
constexpr std::string_view PROPERTY_CACHE_PATH           = WE_SCENE_PROPERTY_CACHE_PATH;
constexpr std::string_view PROPERTY_FIRST_FRAME_CALLBACK = WE_SCENE_PROPERTY_FIRST_FRAME_CALLBACK;
constexpr std::string_view PROPERTY_LOAD_USER_PROPERTIES = WE_SCENE_PROPERTY_LOAD_USER_PROPERTIES;
constexpr std::string_view PROPERTY_USER_PROPERTIES      = WE_SCENE_PROPERTY_USER_PROPERTIES;
constexpr std::string_view PROPERTY_AUDIO_SAMPLES        = WE_SCENE_PROPERTY_AUDIO_SAMPLES;
constexpr std::string_view PROPERTY_CAPTURE_FRAME        = WE_SCENE_PROPERTY_CAPTURE_FRAME;
constexpr std::string_view PROPERTY_CAPTURE_FRAME_NUMBER = WE_SCENE_PROPERTY_CAPTURE_FRAME_NUMBER;

#include "core/NoCopyMove.hpp"
class MainHandler;
struct RenderInitInfo;

class WESceneRuntimeDriver : NoCopy {
public:
    explicit WESceneRuntimeDriver(std::shared_ptr<HostServices> hostServices = {},
                                  std::shared_ptr<WESceneEngineServices> engineServices = {});
    ~WESceneRuntimeDriver();
    bool init();
    bool inited() const;

    void initVulkan(const RenderInitInfo&);

    // Stash a binding that should receive the ex_swapchain once Vulkan
    // init completes. The init path is async (CMD_INIT_VULKAN is posted
    // to the render looper), so the swapchain pointer is null at the
    // time bindOutput() is called; this defers the attach to after init.
    void deferBindingAttach(std::weak_ptr<WESceneOutputBinding> binding);

    void play();
    void pause();
    void mouseInput(double x, double y);
    void mouseButton(bool down);

    void setPropertyBool(std::string_view, bool);
    void setPropertyInt32(std::string_view, int32_t);
    void setPropertyFloat(std::string_view, float);
    void setPropertyString(std::string_view, std::string);
    void setPropertyObject(std::string_view, std::shared_ptr<void>);

    ExSwapchain* exSwapchain() const;

private:
    bool m_inited { false };

private:
    friend class MainHandler;

    bool                                  m_offscreen { false };
    std::shared_ptr<HostServices>         m_hostServices;
    std::shared_ptr<WESceneEngineServices> m_engineServices;
    std::shared_ptr<MainHandler>          m_main_handler;
};
} // namespace wallpaper
