#include "backend/scene/internal/text/WPTextLayer.hpp"

#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneNode.h"

#include <cassert>
#include <memory>

int main() {
    wallpaper::TextLayerRuntimeState state;
    state.object.id = 12;
    state.object.name = "Label";
    state.object.text = "before";
    state.object.size = { 100.0f, 40.0f };
    state.object.horizontalalign = "center";
    state.object.verticalalign = "middle";
    state.primitive = std::make_shared<wallpaper::SceneTextPrimitive>();
    state.primitive->object = state.object;

    assert(wallpaper::HasTextLayerProperty("text"));
    assert(wallpaper::ResolveTextLayerSceneAlignment(state.object) == "center");

    const auto text = wallpaper::ReadTextLayerProperty(state, "text");
    assert(text.has_value());
    std::string text_value;
    assert(text->tryGet(&text_value));
    assert(text_value == "before");

    assert(wallpaper::ApplyTextLayerPropertyValue(
        state, "text", wallpaper::WPDynamicValue(std::string("after"))));
    assert(state.object.text == "after");
    assert(state.primitive->object.text == "after");

    assert(wallpaper::ApplyTextLayerPropertyValue(
        state,
        "size",
        wallpaper::WPDynamicValue(std::array<float, 2> { 200.0f, 50.0f })));
    assert(state.object.size[0] == 200.0f);
    assert(state.object.size_explicit);
    assert(state.primitive->layout.visible_display_size[1] == 50.0f);

    wallpaper::Scene scene;
    scene.textLayers[12] = state;

    wallpaper::SceneNode node;
    assert(wallpaper::ApplyTextLayerTransformValue(
        scene,
        12,
        &node,
        "origin",
        std::array<float, 3> { 3.0f, 4.0f, 5.0f }));
    assert(scene.textLayers[12].object.origin[0] == 3.0f);
    assert(node.Translate().x() == 3.0f);

    assert(wallpaper::RebuildTextLayerSceneLayout(scene, 12));
    assert(scene.dirtyTextLayerIds.count(12) == 1);

    return 0;
}
