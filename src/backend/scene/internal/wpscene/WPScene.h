#pragma once
#include <cstdint>
#include <unordered_map>
#include <cstdint>
#include <string>
#include <vector>
#include "resources/WPJson.hpp"
#include <nlohmann/json.hpp>

namespace wallpaper
{

namespace wpscene
{

class Orthogonalprojection {
public:
    bool    FromJson(const nlohmann::json&);
    int32_t width;
    int32_t height;
    bool    auto_ { false };
};

class WPSceneCamera {
public:
    bool                 FromJson(const nlohmann::json&);
    std::array<float, 3> center { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> eye { 0.0f, 0.0f, 1.0f };
    std::array<float, 3> up { 0.0f, 1.0f, 0.0f };
    // Authored camera path files drive only the model perspective camera. The parser intentionally
    // keeps this data inert until WPModelObject support opts in, preserving legacy 2D camera setup.
    std::vector<std::string> paths;
};

class WPSceneGeneral {
public:
    bool                 FromJson(const nlohmann::json&);
    std::array<float, 3> clearcolor { 0.0f, 0.0f, 0.0f };
    // Wallpaper Engine stores Bloom in scene `general`, not in a layer effect list. Keep both the
    // ordinary LDR Bloom values and the authored HDR metadata here, but the parser must not treat
    // `hdr` as an active render feature until a real HDR pipeline/tone-map path is implemented.
    bool                 bloom { false };
    float                bloomstrength { 0.0f };
    float                bloomthreshold { 1.0f };
    std::array<float, 3> bloomtint { 1.0f, 1.0f, 1.0f };
    bool                 hdr { false };
    float                bloomhdrstrength { 0.0f };
    float                bloomhdrthreshold { 1.0f };
    float                bloomhdrscatter { 1.0f };
    float                bloomhdrfeather { 0.0f };
    int32_t              bloomhdriterations { 0 };
    bool                 cameraparallax { false };
    float                cameraparallaxamount { 0.0f };
    float                cameraparallaxdelay { 0.0f };
    float                cameraparallaxmouseinfluence { 0.0f };
    bool                 isOrtho { true };
    Orthogonalprojection orthogonalprojection { 1920, 1080 };
    float                zoom { 1.0f };
    float                fov { 50.0f };
    float                nearz { 0.01f };
    float                farz { 10000.0f };
    std::array<float, 3> ambientcolor { 0.2f, 0.2f, 0.2f };
    std::array<float, 3> skylightcolor { 0.3f, 0.3f, 0.3f };
};

class WPScene {
public:
    bool           FromJson(const nlohmann::json&);
    WPSceneCamera  camera;
    WPSceneGeneral general;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Orthogonalprojection, width, height);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPSceneCamera, center, eye, up, paths);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPSceneGeneral, clearcolor, orthogonalprojection, zoom);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPScene, camera, general);
} // namespace wpscene
} // namespace wallpaper
