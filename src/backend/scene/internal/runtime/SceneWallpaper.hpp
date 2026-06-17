#pragma once

// Internal driver that isolates the older scene/render pipeline behind the backend interface.
// New integrations should prefer WallpaperSession plus api/scene/WEScene.hpp helpers.

#include <memory>
#include <string_view>
#include <functional>
#include "common/scene/WESceneContract.hpp"
#include "Type.hpp"
#include "swapchain/ExSwapchain.hpp"

namespace wallpaper
{

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

#include "core/NoCopyMove.hpp"
class MainHandler;
struct RenderInitInfo;

class SceneWallpaper : NoCopy {
public:
    SceneWallpaper();
    ~SceneWallpaper();
    bool init();
    bool inited() const;

    void initVulkan(const RenderInitInfo&);

    void play();
    void pause();
    void mouseInput(double x, double y);

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

    bool                         m_offscreen { false };
    std::shared_ptr<MainHandler> m_main_handler;
};
} // namespace wallpaper
