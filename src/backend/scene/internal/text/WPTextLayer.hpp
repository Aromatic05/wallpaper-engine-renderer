#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>
#include <memory>

#include "Image.hpp"
#include "scene/SceneTextPrimitive.h"
#include "WPDynamicValue.hpp"
#include "wpscene/WPTextObject.h"

namespace wallpaper
{

class Scene;
class SceneMesh;
class SceneNode;

namespace fs
{
class VFS;
}

struct TextRasterLayoutResult {
    struct GlyphPage {
        std::shared_ptr<Image> image;
        std::array<float, 2>   source_size { 0.0f, 0.0f };
    };
    struct GlyphQuad {
        uint32_t             page_index { 0 };
        std::array<float, 4> source_rect { 0.0f, 0.0f, 0.0f, 0.0f };
        std::array<float, 4> atlas_rect { 0.0f, 0.0f, 0.0f, 0.0f };
    };
    // `logical_size` describes the authored text box in scene/display units.
    std::array<float, 2>   logical_size { 0.0f, 0.0f };
    // `logical_source_size` tracks the full logical backing resolution even when the glyph bitmap
    // itself is cropped smaller. We keep this around so the runtime can still reconstruct the
    // authored text-box bounds when the optional opaque background becomes visible.
    std::array<float, 2>   logical_source_size { 0.0f, 0.0f };
    // The glyph texture itself is still cropped to the minimal useful rectangle to avoid uploading
    // transparent pixels. These metrics describe the inner glyph content inside the canonical text
    // primitive, rather than a separate image-backed child layer.
    std::array<float, 2>   glyph_display_size { 0.0f, 0.0f };
    std::array<float, 2>   glyph_source_size { 0.0f, 0.0f };
    std::array<float, 2>   glyph_offset { 0.0f, 0.0f };
    // `display_offset` records where cropped glyph bounds sit inside the authored logical box.
    // Node placement keeps the pivot on `logical_size`; glyph/final-output meshes consume resolved
    // local centers derived from this crop delta and the authored alignment/origin.
    std::array<float, 2>   display_offset { 0.0f, 0.0f };
    // The new renderer keeps glyph raster data in atlas pages and stores per-glyph quads separately
    // from the logical layer box. Runtime updates can now rebuild only the page meshes/materials
    // instead of reusing a monolithic full-text image quad.
    std::vector<GlyphPage>  glyph_pages;
    std::vector<GlyphQuad>  glyph_quads;
};

struct TextLayerRuntimeState {
    // Runtime text ownership now flows through a first-class scene primitive. The registry entry
    // only keeps authored state plus the live primitive pointer that currently represents the
    // applied scene content. That way parser, scripts, and the render graph all converge on one
    // canonical geometry source instead of carrying duplicated layout metrics in parallel.
    wpscene::WPTextObject object;
    std::shared_ptr<SceneTextPrimitive> primitive;
    // Property application happens before scene synchronization. Keeping the last applied
    // alignment lets runtime updates compute "previous geometry" from the live primitive even
    // after the authored object has already been mutated to its next state.
    std::string applied_alignment { "center" };
};

enum class TextLayerPropertyUpdateStrategy
{
    LayoutOnly,
    MaterialOnly,
    TransformOnly,
    BridgeResourceResize,
};

std::string ResolveTextLayerSceneAlignment(const wpscene::WPTextObject& object);
TextLayerPropertyUpdateStrategy ResolveTextLayerPropertyUpdateStrategy(
    const TextLayerRuntimeState& state,
    std::string_view             property_name);
bool ApplyTextLayerNodePlacement(SceneNode*                     node,
                                 const TextLayerRuntimeState&   state,
                                 std::array<float, 3>           origin);
bool ApplyTextLayerTransformValue(Scene&                  scene,
                                  int32_t                 layer_id,
                                  SceneNode*              node,
                                  std::string_view        property_name,
                                  std::array<float, 3>    value);
bool ApplyTextLayerScreenAnchorTransforms(Scene& scene);
bool        HasTextLayerProperty(std::string_view property_name);
std::optional<WPDynamicValue> ReadTextLayerProperty(const TextLayerRuntimeState& state,
                                                    std::string_view             property_name);
bool ApplyTextLayerDisplaySize(TextLayerRuntimeState& state,
                               std::array<float, 2>   display_size);
bool ApplyTextLayerPropertyValue(TextLayerRuntimeState& state,
                                 std::string_view       property_name,
                                 const WPDynamicValue&  value);
bool SyncTextLayerSceneMaterials(Scene& scene, int32_t layer_id);
bool RasterizeTextPrimitiveLayout(fs::VFS&                 vfs,
                                  wpscene::WPTextObject&   object,
                                  const std::string&       texture_key,
                                  double                   render_scale,
                                  double                   authoring_scale,
                                  TextRasterLayoutResult*  out_image,
                                  std::string*             out_error = nullptr);
bool BuildSceneTextPrimitive(fs::VFS&                         vfs,
                             wpscene::WPTextObject&           object,
                             uint32_t                         texture_version,
                             double                           render_scale,
                             double                           authoring_scale,
                             std::shared_ptr<SceneTextPrimitive>* out_primitive,
                             std::string*                     out_error = nullptr);
void RebuildTextPrimitiveVisibleMesh(SceneMesh* mesh, const SceneTextPrimitive& primitive);
bool UpdateTextLayerSceneTransform(Scene& scene, int32_t layer_id);
bool UpdateTextLayerSceneBridgeResources(Scene& scene, int32_t layer_id);
bool RebuildTextLayerSceneLayout(Scene& scene, int32_t layer_id);

} // namespace wallpaper
