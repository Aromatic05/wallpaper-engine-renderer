#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/scenescript/WPSceneScriptHost.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

namespace
{
[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "node field animation test failure: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

bool Near(float lhs, float rhs, float epsilon = 0.0001f) {
    return std::abs(lhs - rhs) <= epsilon;
}

std::string AnimatedVec3(std::array<float, 3> base, std::array<float, 3> end) {
    return std::string(R"({
        "value": [)") + std::to_string(base[0]) + "," + std::to_string(base[1]) + "," +
           std::to_string(base[2]) + R"(],
        "animation": {
            "options": {
                "fps": 10,
                "length": 10,
                "mode": "single",
                "name": "node-field"
            },
            "c0": [
                { "frame": 0, "value": )" + std::to_string(base[0]) + R"( },
                { "frame": 10, "value": )" + std::to_string(end[0]) + R"( }
            ],
            "c1": [
                { "frame": 0, "value": )" + std::to_string(base[1]) + R"( },
                { "frame": 10, "value": )" + std::to_string(end[1]) + R"( }
            ],
            "c2": [
                { "frame": 0, "value": )" + std::to_string(base[2]) + R"( },
                { "frame": 10, "value": )" + std::to_string(end[2]) + R"( }
            ]
        }
    })";
}

std::shared_ptr<wallpaper::Scene> ParseAnimatedLight() {
    wallpaper::WPSceneParser parser;
    wallpaper::fs::VFS vfs;
    wallpaper::audio::SoundManager sound_manager;

    const std::string scene_json = std::string(R"({
        "camera": {
            "center": [0, 0, 0],
            "eye": [0, 0, 1],
            "up": [0, 1, 0]
        },
        "general": {
            "clearcolor": [0, 0, 0],
            "orthogonalprojection": {
                "width": 64,
                "height": 64
            },
            "zoom": 1
        },
        "objects": [
            {
                "id": 42,
                "name": "AnimatedLight",
                "light": "point",
                "origin": )") + AnimatedVec3({ 1.0f, 2.0f, 3.0f }, { 11.0f, 12.0f, 13.0f }) + R"(,
                "angles": )" + AnimatedVec3({ 0.0f, 0.0f, 0.0f }, { 1.0f, 2.0f, 3.0f }) + R"(,
                "scale": )" + AnimatedVec3({ 1.0f, 1.0f, 1.0f }, { 3.0f, 5.0f, 7.0f }) + R"(,
                "color": [1, 1, 1],
                "radius": 100,
                "intensity": 1,
                "visible": true
            }
        ]
    })";

    return parser.Parse("node-field-animation", scene_json, vfs, sound_manager);
}
} // namespace

int main() {
    auto scene = ParseAnimatedLight();
    Require(scene != nullptr, "animated light scene failed to parse");
    Require(scene->layerNodes.count(42) == 1, "animated light node was not registered");

    std::unordered_set<std::string> registered_names;
    for (const auto& registration : scene->propertyAnimationRegistrations) {
        if (registration.object_id == 42) registered_names.insert(registration.property_name);
    }
    Require(registered_names.count("origin") == 1,
            "light origin animation was not registered");
    Require(registered_names.count("angles") == 1,
            "light angles animation was not registered");
    Require(registered_names.count("scale") == 1,
            "light scale animation was not registered");

    auto host = std::make_shared<wallpaper::WPSceneScriptHost>(scene.get());
    for (const auto& registration : scene->propertyAnimationRegistrations) {
        Require(host->RegisterPropertyAnimation(registration),
                "node property animation registration failed");
    }
    host->Initialize();
    host->FrameBegin(0.5);

    auto* node = scene->layerNodes.at(42);
    Require(node != nullptr, "animated light node is null");
    const auto& origin = node->Translate();
    const auto& angles = node->Rotation();
    const auto& scale = node->Scale();

    Require(Near(origin.x(), 6.0f) && Near(origin.y(), 7.0f) && Near(origin.z(), 8.0f),
            "origin animation did not reach its midpoint");
    Require(Near(angles.x(), 0.5f) && Near(angles.y(), 1.0f) && Near(angles.z(), 1.5f),
            "angles animation did not reach its midpoint");
    Require(Near(scale.x(), 2.0f) && Near(scale.y(), 3.0f) && Near(scale.z(), 4.0f),
            "scale animation did not reach its midpoint");

    return 0;
}
