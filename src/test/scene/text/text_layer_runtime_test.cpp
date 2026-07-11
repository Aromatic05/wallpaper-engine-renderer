#include "backend/scene/internal/text/WPTextLayer.hpp"

#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneNode.h"
#include "common/fs/include/fs/VFS.h"

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

    wallpaper::fs::VFS vfs;
    std::string error;
    assert(wallpaper::BuildSceneTextPrimitive(vfs, state.object, 1, 1.0, 1.0, &state.primitive, &error));
    assert(state.primitive != nullptr);
    assert(!state.primitive->layout.glyph_pages.empty());
    assert(!state.primitive->layout.glyph_runs.empty());
    assert(!state.primitive->glyph_pages.empty());
    assert(state.primitive->glyph_pages.front().mesh != nullptr);

    assert(wallpaper::HasTextLayerProperty("text"));
    assert(wallpaper::ResolveTextLayerSceneAlignment(state.object).empty());

    const auto text = wallpaper::ReadTextLayerProperty(state, "text");
    assert(text.has_value());
    std::string text_value;
    assert(text->tryGet(&text_value));
    assert(text_value == "before");

    assert(wallpaper::ApplyTextLayerPropertyValue(
        state, "text", wallpaper::WPDynamicValue(std::string("after"))));
    assert(state.object.text == "after");
    assert(state.primitive->object.text == "before");

    assert(wallpaper::ApplyTextLayerDisplaySize(state, { 200.0f, 50.0f }));
    assert(state.object.size[0] == 200.0f);
    assert(state.object.size_explicit);
    assert(state.primitive->layout.visible_display_size[1] != 50.0f);

    wallpaper::Scene scene;
    scene.vfs = std::make_unique<wallpaper::fs::VFS>();
    scene.textLayers[12] = state;
    scene.textPrimitives[12] = state.primitive;

    wallpaper::SceneNode node;
    node.ID() = 12;
    node.AddText(state.primitive);
    scene.layerNodes[12] = &node;
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
    assert(scene.renderGraphResourcesDirty);
    assert(scene.textLayers[12].primitive->object.text == "after");
    assert(scene.textLayers[12].primitive->atlas_version == 2);
    assert(!scene.textLayers[12].primitive->layout.glyph_pages.empty());
    assert(!scene.textLayers[12].primitive->glyph_pages.empty());
    scene.MarkRenderTargetResourcesDirty("_rt_text");
    assert(scene.dirtyRenderTargetKeys.count("_rt_text") == 1);
    scene.ClearRenderGraphDirty();
    assert(!scene.renderGraphTopologyDirty);
    assert(!scene.renderGraphResourcesDirty);
    assert(scene.dirtyTextLayerIds.empty());
    assert(scene.dirtyRenderTargetKeys.empty());

    return 0;
}
