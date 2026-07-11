#include "backend/scene/internal/scenescript/WPSceneScriptHost.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneNode.h"

namespace
{

void Require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

std::shared_ptr<wallpaper::SceneNode> MakeLayerNode(int32_t id, std::string name) {
    auto node = std::make_shared<wallpaper::SceneNode>();
    node->ID() = id;
    node->SetName(std::move(name));
    return node;
}

} // namespace

int main() {
    wallpaper::Scene scene;
    scene.ortho[0] = 320.0f;
    scene.ortho[1] = 180.0f;

    auto node = MakeLayerNode(56, "ViewportLayer");
    scene.sceneGraph->AppendChild(node);
    scene.layerOrder.push_back(56);
    scene.layerNodes[56] = node.get();
    scene.layerNameToId[node->Name()] = 56;
    scene.initialLayerConfigJson[56] = R"({"id":56,"name":"ViewportLayer"})";
    scene.objectRuntimeNodes[56].push_back(node.get());
    scene.nodeOwners[node.get()] = 56;

    wallpaper::WPSceneScriptRegistration registration;
    registration.object_id = 56;
    registration.object_name = "ViewportLayer";
    registration.property_name = "visible";
    registration.node = node.get();
    registration.target_kind = wallpaper::WPSceneScriptTargetKind::Layer;
    registration.value_type = wallpaper::WPDynamicValue::Type::Boolean;
    registration.base_value = wallpaper::WPDynamicValue(true);
    registration.setting.value = registration.base_value;
    registration.setting.script = R"(
        export function init(value) {
            const canvasCenter = engine.canvasSize.divide(2);
            const screenCenter = engine.screenResolution.divide(2);
            shared.viewportVectorsValid =
                engine.canvasSize.x === 320 && engine.screenResolution.x === 320 &&
                canvasCenter.x === 160 && canvasCenter.y === 90 &&
                screenCenter.x === 160 && screenCenter.y === 90;
            return value;
        }
        export function update(value) {
            return shared.viewportVectorsValid;
        }
    )";

    wallpaper::WPSceneScriptHost host(&scene);
    Require(host.Ready(), "SceneScript host should initialize");
    Require(host.RegisterPropertyScript(std::move(registration)),
            "viewport vector script should register");
    host.Initialize();
    host.FrameBegin(0.1);
    Require(scene.IsLayerVisible(56),
            "canvasSize and screenResolution should expose Vec2 methods and current dimensions");
    return 0;
}
