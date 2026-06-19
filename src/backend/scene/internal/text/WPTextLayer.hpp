#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <memory>

#include "settings/WPDynamicValue.hpp"
#include "scene/SceneTextPrimitive.h"
#include "wpscene/WPTextObject.h"

namespace wallpaper
{

class Scene;
class SceneNode;

struct TextLayerRuntimeState {
    wpscene::WPTextObject object;
    std::shared_ptr<SceneTextPrimitive> primitive;
    std::string applied_alignment { "center" };
};

enum class TextLayerPropertyUpdateStrategy
{
    LayoutOnly,
    MaterialOnly,
    TransformOnly,
};

std::string ResolveTextLayerSceneAlignment(const wpscene::WPTextObject& object);
TextLayerPropertyUpdateStrategy ResolveTextLayerPropertyUpdateStrategy(
    const TextLayerRuntimeState& state,
    std::string_view             property_name);

bool HasTextLayerProperty(std::string_view property_name);
std::optional<WPDynamicValue> ReadTextLayerProperty(const TextLayerRuntimeState& state,
                                                    std::string_view             property_name);
bool ApplyTextLayerDisplaySize(TextLayerRuntimeState& state,
                               std::array<float, 2>   display_size);
bool ApplyTextLayerPropertyValue(TextLayerRuntimeState& state,
                                 std::string_view       property_name,
                                 const WPDynamicValue&  value);
bool ApplyTextLayerNodePlacement(SceneNode*                   node,
                                 const TextLayerRuntimeState& state,
                                 std::array<float, 3>         origin);
bool ApplyTextLayerTransformValue(Scene&               scene,
                                  int32_t              layer_id,
                                  SceneNode*           node,
                                  std::string_view     property_name,
                                  std::array<float, 3> value);
bool SyncTextLayerSceneMaterials(Scene& scene, int32_t layer_id);
bool RebuildTextLayerSceneLayout(Scene& scene, int32_t layer_id);

} // namespace wallpaper
