#include "WPTextLayer.hpp"

#include <algorithm>

#include "scene/Scene.h"
#include "scene/SceneNode.h"

namespace wallpaper
{
namespace
{

std::array<int32_t, 4> UniformTextPadding(int32_t value) {
    return { value, value, value, value };
}

std::string ResolveTextContentAlignment(const wpscene::WPTextObject& object) {
    std::string alignment;
    if (object.verticalalign == "bottom") {
        alignment += "bottom";
    } else if (object.verticalalign == "middle" || object.verticalalign == "center") {
        alignment += "center";
    } else {
        alignment += "top";
    }

    if (object.horizontalalign == "right") {
        alignment += "right";
    } else if (object.horizontalalign == "center" || object.horizontalalign == "middle") {
        if (alignment != "center") alignment += "center";
    } else {
        alignment += "left";
    }
    return alignment;
}

void SyncPrimitiveFromState(TextLayerRuntimeState& state) {
    if (state.primitive == nullptr) return;

    state.primitive->object = state.object;
    state.primitive->layout.logical_size = state.object.size;
    state.primitive->layout.logical_source_size = state.object.size;
    state.primitive->layout.visible_display_size = state.object.size;
    state.primitive->layout.visible_source_size = state.object.size;
    state.primitive->layout.visible_display_offset = { 0.0f, 0.0f };
    state.applied_alignment = ResolveTextLayerSceneAlignment(state.object);
}

} // namespace

std::string ResolveTextLayerSceneAlignment(const wpscene::WPTextObject& object) {
    if (! object.anchor.empty() && object.anchor != "none" && object.anchor != "center") {
        return object.anchor;
    }
    return ResolveTextContentAlignment(object);
}

TextLayerPropertyUpdateStrategy ResolveTextLayerPropertyUpdateStrategy(
    const TextLayerRuntimeState& state,
    std::string_view             property_name) {
    (void)state;
    if (property_name == "alpha" || property_name == "color" ||
        property_name == "backgroundcolor" || property_name == "backgroundbrightness") {
        return TextLayerPropertyUpdateStrategy::MaterialOnly;
    }
    if (property_name == "anchor") return TextLayerPropertyUpdateStrategy::TransformOnly;
    return TextLayerPropertyUpdateStrategy::LayoutOnly;
}

bool HasTextLayerProperty(std::string_view property_name) {
    return property_name == "name" || property_name == "size" || property_name == "text" ||
           property_name == "font" || property_name == "color" || property_name == "alpha" ||
           property_name == "backgroundcolor" || property_name == "backgroundbrightness" ||
           property_name == "opaquebackground" || property_name == "pointsize" ||
           property_name == "padding" || property_name == "horizontalalign" ||
           property_name == "verticalalign" || property_name == "anchor" ||
           property_name == "limitrows" || property_name == "maxrows" ||
           property_name == "limitwidth" || property_name == "maxwidth";
}

std::optional<WPDynamicValue> ReadTextLayerProperty(const TextLayerRuntimeState& state,
                                                    std::string_view property_name) {
    const auto& object = state.object;
    if (property_name == "name") return WPDynamicValue(object.name);
    if (property_name == "size") return WPDynamicValue(object.size);
    if (property_name == "text") return WPDynamicValue(object.text);
    if (property_name == "font") return WPDynamicValue(object.font);
    if (property_name == "color") return WPDynamicValue(object.color);
    if (property_name == "alpha") return WPDynamicValue(object.alpha);
    if (property_name == "backgroundcolor") return WPDynamicValue(object.backgroundcolor);
    if (property_name == "backgroundbrightness") return WPDynamicValue(object.backgroundbrightness);
    if (property_name == "opaquebackground") return WPDynamicValue(object.opaquebackground);
    if (property_name == "pointsize") return WPDynamicValue(object.pointsize);
    if (property_name == "padding") return WPDynamicValue(static_cast<int32_t>(object.padding));
    if (property_name == "horizontalalign") return WPDynamicValue(object.horizontalalign);
    if (property_name == "verticalalign") return WPDynamicValue(object.verticalalign);
    if (property_name == "anchor") return WPDynamicValue(object.anchor);
    if (property_name == "limitrows") return WPDynamicValue(object.limitrows);
    if (property_name == "maxrows") return WPDynamicValue(static_cast<int32_t>(object.maxrows));
    if (property_name == "limitwidth") return WPDynamicValue(object.limitwidth);
    if (property_name == "maxwidth") return WPDynamicValue(object.maxwidth);
    return std::nullopt;
}

bool ApplyTextLayerDisplaySize(TextLayerRuntimeState& state,
                               std::array<float, 2>   display_size) {
    display_size[0] = std::max(display_size[0], 1.0f);
    display_size[1] = std::max(display_size[1], 1.0f);
    state.object.size = display_size;
    state.object.size_explicit = true;
    SyncPrimitiveFromState(state);
    return true;
}

bool ApplyTextLayerPropertyValue(TextLayerRuntimeState& state,
                                 std::string_view       property_name,
                                 const WPDynamicValue&  value) {
    auto& object = state.object;
    bool  applied { false };

    if (property_name == "name") {
        applied = value.tryGet(&object.name);
    } else if (property_name == "size") {
        std::array<float, 2> size {};
        if (! value.tryGet(&size)) return false;
        return ApplyTextLayerDisplaySize(state, size);
    } else if (property_name == "text") {
        applied = value.tryGet(&object.text);
    } else if (property_name == "font") {
        applied = value.tryGet(&object.font);
    } else if (property_name == "color") {
        applied = value.tryGet(&object.color);
    } else if (property_name == "alpha") {
        applied = value.tryGet(&object.alpha);
    } else if (property_name == "backgroundcolor") {
        applied = value.tryGet(&object.backgroundcolor);
    } else if (property_name == "backgroundbrightness") {
        applied = value.tryGet(&object.backgroundbrightness);
    } else if (property_name == "opaquebackground") {
        applied = value.tryGet(&object.opaquebackground);
    } else if (property_name == "pointsize") {
        applied = value.tryGet(&object.pointsize);
    } else if (property_name == "padding") {
        int32_t padding { 0 };
        if (! value.tryGet(&padding)) return false;
        object.padding = padding;
        object.padding_edges = UniformTextPadding(padding);
        applied = true;
    } else if (property_name == "horizontalalign") {
        applied = value.tryGet(&object.horizontalalign);
    } else if (property_name == "verticalalign") {
        applied = value.tryGet(&object.verticalalign);
    } else if (property_name == "anchor") {
        applied = value.tryGet(&object.anchor);
    } else if (property_name == "limitrows") {
        applied = value.tryGet(&object.limitrows);
    } else if (property_name == "maxrows") {
        applied = value.tryGet(&object.maxrows);
    } else if (property_name == "limitwidth") {
        applied = value.tryGet(&object.limitwidth);
    } else if (property_name == "maxwidth") {
        applied = value.tryGet(&object.maxwidth);
    }

    if (applied) SyncPrimitiveFromState(state);
    return applied;
}

bool ApplyTextLayerNodePlacement(SceneNode*                   node,
                                 const TextLayerRuntimeState& state,
                                 std::array<float, 3>         origin) {
    if (node == nullptr) return false;

    (void)state;
    node->SetTranslate(Eigen::Vector3f { origin[0], origin[1], origin[2] });
    return true;
}

bool ApplyTextLayerTransformValue(Scene&               scene,
                                  int32_t              layer_id,
                                  SceneNode*           node,
                                  std::string_view     property_name,
                                  std::array<float, 3> value) {
    auto state_it = scene.textLayers.find(layer_id);
    if (state_it == scene.textLayers.end() || node == nullptr) return false;

    auto& state = state_it->second;
    if (property_name == "origin") {
        state.object.origin = value;
        return ApplyTextLayerNodePlacement(node, state, value);
    }
    if (property_name == "angles") {
        state.object.angles = value;
        node->SetRotation(Eigen::Vector3f { value[0], value[1], value[2] });
        return true;
    }
    if (property_name == "scale") {
        state.object.scale = value;
        node->SetScale(Eigen::Vector3f { value[0], value[1], value[2] });
        return true;
    }
    return false;
}

bool SyncTextLayerSceneMaterials(Scene& scene, int32_t layer_id) {
    auto state_it = scene.textLayers.find(layer_id);
    if (state_it == scene.textLayers.end() || state_it->second.primitive == nullptr) return false;

    state_it->second.primitive->object = state_it->second.object;
    return true;
}

bool RebuildTextLayerSceneLayout(Scene& scene, int32_t layer_id) {
    auto state_it = scene.textLayers.find(layer_id);
    if (state_it == scene.textLayers.end() || state_it->second.primitive == nullptr) return false;

    SyncPrimitiveFromState(state_it->second);
    scene.MarkTextLayerResourcesDirty(layer_id);
    return true;
}

} // namespace wallpaper
