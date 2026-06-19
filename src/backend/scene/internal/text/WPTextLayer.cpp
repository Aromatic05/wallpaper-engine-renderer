#include "WPTextLayer.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>

#include <Eigen/Geometry>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "fs/VFS.h"
#include "scene/SceneCamera.h"
#include "scene/SceneImageEffectLayer.h"
#include "scene/SceneNode.h"
#include "scene/Scene.h"
#include "SpecTexs.hpp"
#include "utils/Sha.hpp"
#include "WPSceneScriptMedia.hpp"

using namespace wallpaper;

namespace
{

constexpr std::string_view kFontCacheDir { "/tmp/hanabi-scene-font-cache" };
constexpr double kBaseTextResolutionDpi { 96.0 };
constexpr float  kMinTextVisualScaleFactor { 0.0625f };
constexpr int    kTextGlyphAtlasMaxExtent { 1024 };
constexpr int    kTextGlyphAtlasPadding { 1 };
constexpr float  kScreenAnchoredTextStackGap { 1.0f };
constexpr float  kTextPlacementEpsilon { 0.000001f };

uint16_t ResolveTextMeshExtent(float value) {
    return static_cast<uint16_t>(std::max(1, static_cast<int>(std::lround(value))));
}

void GenCardMesh(SceneMesh&                    mesh,
                 const std::array<uint16_t, 2> size) {
    const float left = -(size[0] / 2.0f);
    const float right = size[0] / 2.0f;
    const float bottom = -(size[1] / 2.0f);
    const float top = size[1] / 2.0f;
    const float z = 0.0f;

    // The first-class text primitive still uses quad meshes for the background card and bridge
    // outputs. Keeping a local copy of this helper inside WPTextLayer avoids coupling the new text
    // pipeline to parser-only helpers that are going away as part of the rewrite.
    const std::array<float, 12> positions {
        left, bottom, z,
        left, top, z,
        right, bottom, z,
        right, top, z,
    };
    const std::array<float, 8> texcoords { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f };

    SceneVertexArray vertex(
        {
            { WE_IN_POSITION.data(), VertexType::FLOAT3 },
            { WE_IN_TEXCOORD.data(), VertexType::FLOAT2 },
        },
        4);
    vertex.SetVertex(WE_IN_POSITION, positions);
    vertex.SetVertex(WE_IN_TEXCOORD, texcoords);
    mesh.AddVertexArray(std::move(vertex));
}

const nlohmann::json* ResolveTextPropertyValueNode(const nlohmann::json& json) {
    if (! json.is_object()) return &json;
    if (json.contains("value") && ! json.at("value").is_null()) return &json.at("value");

    if (json.contains("animation") && json.at("animation").is_object()) {
        const auto& animation    = json.at("animation");
        bool        start_paused = false;
        if (animation.contains("options") && animation.at("options").is_object() &&
            animation.at("options").contains("startpaused") &&
            animation.at("options").at("startpaused").is_boolean()) {
            start_paused = animation.at("options").at("startpaused").get<bool>();
        }

        if (start_paused && animation.contains("c0") && animation.at("c0").is_array() &&
            ! animation.at("c0").empty() && animation.at("c0").front().is_object() &&
            animation.at("c0").front().contains("value") &&
            ! animation.at("c0").front().at("value").is_null()) {
            return &animation.at("c0").front().at("value");
        }
    }

    return nullptr;
}

template<typename T>
void ReadLiteralOrDynamicValue(const nlohmann::json& json, const char* name, T* out_value) {
    if (out_value == nullptr || ! json.contains(name) || json.at(name).is_null()) return;

    const auto* value_node = ResolveTextPropertyValueNode(json.at(name));
    if (value_node == nullptr) return;

    GET_JSON_VALUE_NOWARN(*value_node, *out_value);
}

std::array<int32_t, 4> UniformTextPadding(int32_t value) {
    return { value, value, value, value };
}

int32_t MaxTextPaddingEdge(const std::array<int32_t, 4>& padding) {
    return std::max({ padding[0], padding[1], padding[2], padding[3] });
}

std::array<int32_t, 4> ClampTextPaddingEdges(std::array<int32_t, 4> padding) {
    for (auto& edge : padding) edge = std::max(edge, 0);
    return padding;
}

int TextPaddingHorizontal(const std::array<int32_t, 4>& padding) {
    return padding[3] + padding[1];
}

int TextPaddingVertical(const std::array<int32_t, 4>& padding) {
    return padding[0] + padding[2];
}

bool ParseTextPaddingComponentString(std::string_view text, std::vector<double>* out_components) {
    if (out_components == nullptr) return false;

    std::istringstream input { std::string(text) };
    std::vector<double> components;
    double component = 0.0;
    while (input >> component) components.push_back(component);
    input >> std::ws;
    if (!input.eof() || components.empty()) return false;

    *out_components = std::move(components);
    return true;
}

bool ReadTextPaddingComponents(const nlohmann::json& value_node,
                               std::vector<double>* out_components) {
    if (out_components == nullptr) return false;

    if (value_node.is_number()) {
        *out_components = { value_node.get<double>() };
        return true;
    }

    if (value_node.is_string()) {
        return ParseTextPaddingComponentString(value_node.get_ref<const std::string&>(),
                                               out_components);
    }

    if (value_node.is_array()) {
        std::vector<double> components;
        components.reserve(value_node.size());
        for (const auto& item : value_node) {
            if (item.is_number()) {
                components.push_back(item.get<double>());
                continue;
            }
            if (item.is_string()) {
                std::vector<double> item_components;
                if (!ParseTextPaddingComponentString(item.get_ref<const std::string&>(),
                                                     &item_components)) {
                    return false;
                }
                components.insert(components.end(), item_components.begin(), item_components.end());
                continue;
            }
            return false;
        }
        if (components.empty()) return false;
        *out_components = std::move(components);
        return true;
    }

    return false;
}

int32_t RoundTextPaddingComponent(double value) {
    if (!std::isfinite(value)) return 0;
    return static_cast<int32_t>(std::lround(value));
}

std::optional<std::array<int32_t, 4>> ExpandTextPaddingComponents(
    const std::vector<double>& components) {
    if (components.empty() || components.size() > 4) return std::nullopt;

    const auto edge = [&](size_t index) { return RoundTextPaddingComponent(components[index]); };

    // Text padding is a shorthand, not a scalar. Keep the same expansion model as CSS:
    // one value applies to every edge; two values are vertical/horizontal; three values are
    // top/horizontal/bottom; four values are top/right/bottom/left.
    if (components.size() == 1) return UniformTextPadding(edge(0));
    if (components.size() == 2) return { { edge(0), edge(1), edge(0), edge(1) } };
    if (components.size() == 3) return { { edge(0), edge(1), edge(2), edge(1) } };
    return { { edge(0), edge(1), edge(2), edge(3) } };
}

void ReadTextPaddingValue(const nlohmann::json& json,
                          int32_t              object_id,
                          int32_t*             out_legacy_padding,
                          std::array<int32_t, 4>* out_padding_edges) {
    if (out_legacy_padding == nullptr || out_padding_edges == nullptr ||
        !json.contains("padding") || json.at("padding").is_null()) {
        return;
    }

    const auto* value_node = ResolveTextPropertyValueNode(json.at("padding"));
    if (value_node == nullptr) return;

    std::vector<double> components;
    if (!ReadTextPaddingComponents(*value_node, &components)) {
        const std::string raw = value_node->dump();
        LOG_ERROR("TextPaddingParse: layer=%d unsupported padding=%s",
                  object_id,
                  raw.c_str());
        return;
    }

    const auto expanded = ExpandTextPaddingComponents(components);
    if (!expanded.has_value()) {
        const std::string raw = value_node->dump();
        LOG_ERROR("TextPaddingParse: layer=%d invalid component-count=%zu padding=%s",
                  object_id,
                  components.size(),
                  raw.c_str());
        return;
    }

    *out_padding_edges  = ClampTextPaddingEdges(*expanded);
    *out_legacy_padding = MaxTextPaddingEdge(*out_padding_edges);
}

std::string NormalizeAssetPath(fs::VFS& vfs, std::string_view path) {
    if (path.empty()) return {};
    if (path.starts_with('/')) return std::string(path);

    const std::string asset_path = std::string("/assets/") + std::string(path);
    if (vfs.Contains(asset_path)) return asset_path;
    if (vfs.Contains(path)) return std::string(path);
    return asset_path;
}

std::string NormalizeSystemFontAlias(std::string font) {
    if (! font.starts_with("systemfont_")) return font;

    font.erase(0, std::string("systemfont_").size());
    std::replace(font.begin(), font.end(), '_', ' ');
    if (font == "default") return "Sans";
    return font.empty() ? std::string("Sans") : font;
}

std::string LowercaseAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool IsSupportedFontAssetPath(std::string_view path) {
    const auto extension =
        LowercaseAscii(std::filesystem::path(std::string(path)).extension().string());
    return extension == ".ttf" || extension == ".otf";
}

struct AssetFontCacheEntry {
    std::string request_path;
    std::string asset_path;
    std::string family;
    std::string temp_font_path;
    std::string content_hash;
    size_t      byte_count { 0 };
};

struct AssetFontCacheState {
    std::mutex mutex;
    std::unordered_map<std::string, AssetFontCacheEntry> entries;
    std::unordered_map<std::string, std::string> scene_asset_content_keys;
    std::unordered_set<std::string> missing_scene_asset_keys;
    std::unordered_set<std::string> registered_config_font_paths;
};

AssetFontCacheState& GetAssetFontCacheState() {
    static AssetFontCacheState state;
    return state;
}

std::string MakeAssetFontCacheKey(std::string_view request_path,
                                  std::string_view asset_path,
                                  std::string_view content_hash,
                                  size_t           byte_count) {
    // Different wallpapers can reuse the same authored request path and the same virtual VFS asset
    // path for different font bytes. Include the content identity in the lookup key so a scene reuse
    // never returns a family/temp file that belongs to the previous wallpaper.
    std::string key;
    key.reserve(request_path.size() + asset_path.size() + content_hash.size() + 32);
    key.append(request_path);
    key.push_back('\n');
    key.append(asset_path);
    key.push_back('\n');
    key.append(content_hash);
    key.push_back('\n');
    key.append(std::to_string(byte_count));
    return key;
}

std::string MakeSceneAssetFontCacheKey(uint64_t         vfs_identity,
                                       std::string_view request_path,
                                       std::string_view asset_path) {
    // Scene reuse keeps the renderer object alive, but every load builds a new VFS. Keying the fast
    // path by that VFS identity plus the authored and normalized font paths lets runtime text ticks
    // reuse the already resolved family without collapsing two different wallpapers that both use
    // "fonts/foo.ttf" into the same cache entry.
    std::string key;
    key.reserve(request_path.size() + asset_path.size() + 48);
    key.append(std::to_string(vfs_identity));
    key.push_back('\n');
    key.append(request_path);
    key.push_back('\n');
    key.append(asset_path);
    return key;
}

std::string MakeFontConfigRegistrationKey(FcConfig* config, std::string_view temp_font_path) {
    // Fontconfig can expose a different current config to different threads or after toolkit setup
    // changes. Track app-font registration by config pointer plus file path instead of assuming that
    // a process-global "already added" bit is enough.
    return std::to_string(reinterpret_cast<std::uintptr_t>(config)) + "\n" +
           std::string(temp_font_path);
}

std::string MakePangoThreadVisibilityKey(FcConfig*    config,
                                         PangoFontMap* font_map,
                                         std::string_view temp_font_path) {
    // PangoFcFontMap caches Fontconfig state inside the font map owned by the calling thread. The
    // visibility marker therefore includes both the current FcConfig and the current Pango font map,
    // so a cache hit on another render/main thread still refreshes that thread's map before layout.
    return std::to_string(reinterpret_cast<std::uintptr_t>(config)) + "\n" +
           std::to_string(reinterpret_cast<std::uintptr_t>(font_map)) + "\n" +
           std::string(temp_font_path);
}

bool WriteAssetFontTempFile(const AssetFontCacheEntry& entry, std::string_view bytes) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(kFontCacheDir.data()), ec);
    if (ec) {
        LOG_ERROR("failed to create text font temp directory: request=%s asset=%s dir=%s error=%s",
                  entry.request_path.c_str(),
                  entry.asset_path.c_str(),
                  kFontCacheDir.data(),
                  ec.message().c_str());
        return false;
    }

    bool should_write = true;
    const auto existing_size = std::filesystem::file_size(entry.temp_font_path, ec);
    if (! ec && existing_size == entry.byte_count) {
        should_write = false;
    }

    if (should_write) {
        std::ofstream out(entry.temp_font_path, std::ios::binary | std::ios::trunc);
        if (! out) {
            LOG_ERROR("failed to open text font temp file: request=%s asset=%s temp=%s",
                      entry.request_path.c_str(),
                      entry.asset_path.c_str(),
                      entry.temp_font_path.c_str());
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (! out) {
            LOG_ERROR("failed to write text font temp file: request=%s asset=%s temp=%s bytes=%zu",
                      entry.request_path.c_str(),
                      entry.asset_path.c_str(),
                      entry.temp_font_path.c_str(),
                      entry.byte_count);
            return false;
        }
    }

    ec.clear();
    const auto temp_size = std::filesystem::file_size(entry.temp_font_path, ec);
    if (ec || temp_size != entry.byte_count) {
        LOG_ERROR("text font temp file validation failed: request=%s asset=%s temp=%s expected=%zu",
                  entry.request_path.c_str(),
                  entry.asset_path.c_str(),
                  entry.temp_font_path.c_str(),
                  entry.byte_count);
        return false;
    }

    return true;
}

bool EnsureAssetFontVisibleToCurrentPangoFontMap(AssetFontCacheState&        state,
                                                const AssetFontCacheEntry& entry) {
    std::error_code ec;
    const auto      temp_size = std::filesystem::file_size(entry.temp_font_path, ec);
    if (ec || temp_size != entry.byte_count) {
        LOG_ERROR("cached text font temp file is not usable: request=%s asset=%s temp=%s hash=%s bytes=%zu",
                  entry.request_path.c_str(),
                  entry.asset_path.c_str(),
                  entry.temp_font_path.c_str(),
                  entry.content_hash.c_str(),
                  entry.byte_count);
        return false;
    }

    FcInit();
    auto* config = FcConfigGetCurrent();
    if (config == nullptr) {
        LOG_ERROR("Fontconfig current config is unavailable for text font: request=%s asset=%s temp=%s",
                  entry.request_path.c_str(),
                  entry.asset_path.c_str(),
                  entry.temp_font_path.c_str());
        return false;
    }

    const auto registration_key = MakeFontConfigRegistrationKey(config, entry.temp_font_path);
    bool       registered_now   = false;
    if (state.registered_config_font_paths.count(registration_key) == 0) {
        if (! FcConfigAppFontAddFile(config,
                                     reinterpret_cast<const FcChar8*>(entry.temp_font_path.c_str()))) {
            LOG_ERROR("Fontconfig failed to add text font file: request=%s asset=%s temp=%s family=%s",
                      entry.request_path.c_str(),
                      entry.asset_path.c_str(),
                      entry.temp_font_path.c_str(),
                      entry.family.c_str());
            return false;
        }
        if (! FcConfigBuildFonts(config)) {
            LOG_ERROR("Fontconfig failed to rebuild text font set: request=%s asset=%s temp=%s family=%s",
                      entry.request_path.c_str(),
                      entry.asset_path.c_str(),
                      entry.temp_font_path.c_str(),
                      entry.family.c_str());
            return false;
        }
        state.registered_config_font_paths.insert(registration_key);
        registered_now = true;
    }

    auto* font_map = pango_cairo_font_map_get_default();
    if (font_map == nullptr || ! PANGO_IS_FC_FONT_MAP(font_map)) {
        LOG_ERROR("Pango Fc font map is unavailable for text font: request=%s asset=%s temp=%s family=%s",
                  entry.request_path.c_str(),
                  entry.asset_path.c_str(),
                  entry.temp_font_path.c_str(),
                  entry.family.c_str());
        return false;
    }

    thread_local std::unordered_set<std::string> visible_thread_font_maps;
    const auto visibility_key = MakePangoThreadVisibilityKey(config, font_map, entry.temp_font_path);
    if (registered_now || visible_thread_font_maps.count(visibility_key) == 0) {
        // Cache hits still have to pass through this branch on a thread that has not used the temp
        // font before. PangoFcFontMap snapshots Fontconfig state internally, so adding the file to
        // Fontconfig on another thread does not guarantee that this thread's font map will see it.
        pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(font_map));
        visible_thread_font_maps.insert(visibility_key);
    }

    return true;
}

void ApplyTextCairoRenderOptions(cairo_t* cr) {
    if (cr == nullptr) return;
    auto* options = cairo_font_options_create();
    // Text is rendered into transparent textures and later composited by the scene.
    // Gray antialiasing avoids the colored fringes that LCD/subpixel AA produces
    // on transparent surfaces, while slight hinting keeps edges softer.
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_subpixel_order(options, CAIRO_SUBPIXEL_ORDER_DEFAULT);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);
}

void ApplyTextFontRenderOptions(cairo_t* cr, PangoLayout* layout) {
    if (cr == nullptr || layout == nullptr) return;

    ApplyTextCairoRenderOptions(cr);
    auto* options = cairo_font_options_create();
    cairo_get_font_options(cr, options);
    pango_cairo_context_set_font_options(pango_layout_get_context(layout), options);
    pango_cairo_update_layout(cr, layout);

    cairo_font_options_destroy(options);
}

void ApplyTextResolution(PangoLayout* layout) {
    if (layout == nullptr) return;

    auto* context = pango_layout_get_context(layout);
    if (context == nullptr) return;

    // Keep Pango metrics in logical project units. The cairo surface device scale
    // below is what changes backing-texture density for HiDPI and world scaling.
    pango_cairo_context_set_resolution(context, kBaseTextResolutionDpi);
}

void ApplyWallpaperEngineTextSize(PangoFontDescription* desc, double point_size) {
    if (desc == nullptr) return;

    // Wallpaper Engine stores text `pointsize` in authored canvas pixels, while Pango's regular
    // point-size API converts through the 96/72 DPI ratio. Use an absolute Pango size so the glyph
    // metrics stay in the same pixel-space contract as the scene JSON and the Windows renderer.
    pango_font_description_set_absolute_size(desc, std::max(1.0, point_size) * PANGO_SCALE);
}

void ReadVisibleBinding(const nlohmann::json& json, VisibleBinding* binding) {
    if (binding == nullptr || ! json.is_object()) return;

    GET_JSON_NAME_VALUE_NOWARN(json, "value", binding->value);
    if (! json.contains("user") || json.at("user").is_null()) return;

    const auto& user = json.at("user");
    if (user.is_string()) {
        GET_JSON_VALUE(user, binding->user.name);
        return;
    }
    if (! user.is_object()) return;

    GET_JSON_NAME_VALUE_NOWARN(user, "name", binding->user.name);
    GET_JSON_NAME_VALUE_NOWARN(user, "condition", binding->user.condition);
}

PangoAlignment ToPangoAlignment(std::string_view alignment) {
    if (alignment == "center") return PANGO_ALIGN_CENTER;
    if (alignment == "right") return PANGO_ALIGN_RIGHT;
    return PANGO_ALIGN_LEFT;
}

Eigen::Vector3f AlignmentOffset(std::string_view alignment, std::array<float, 2> size);

bool TextLayerUsesMaterialTint(const wpscene::WPTextObject& object) {
    (void)object;
    // Text glyph rasters now always store coverage-only data. Background composition is handled
    // by a dedicated quad, so every text layer can keep glyph color changes on the material path.
    return true;
}

bool TextLayerUsesTightTransparentGlyphBounds(const wpscene::WPTextObject& object) {
    return !object.opaquebackground && object.effects.empty();
}

std::array<int32_t, 4> ResolveTextLayoutPadding(const wpscene::WPTextObject& object) {
    // Wallpaper Engine's padding belongs to text that keeps its authored logical rectangle alive:
    // opaque-background text needs room for the background quad, and effect-backed text needs the
    // extra transparent source pixels that shaders may sample. Plain transparent text, however,
    // exposes tight glyph coverage to the scene; carrying padding through that direct path turns
    // invisible padding into visible placement geometry and moves the crop center away from the
    // authored text placement.
    return TextLayerUsesTightTransparentGlyphBounds(object)
               ? UniformTextPadding(0)
               : ClampTextPaddingEdges(object.padding_edges);
}

bool TextObjectUsesImplicitSceneAlignment(const wpscene::WPTextObject& object) {
    return object.anchor.empty() || object.anchor == "none";
}

bool TextObjectUsesParagraphCenterPivot(const wpscene::WPTextObject& object) {
    return TextObjectUsesImplicitSceneAlignment(object) &&
           object.horizontalalign == "center" && object.verticalalign == "center";
}

bool TextObjectUsesAutoSizedParagraphBox(const wpscene::WPTextObject& object) {
    return !object.limitwidth && !object.limitrows;
}

enum class TextCropCenterProjection
{
    LocalLayoutBox,
    ParentCrossAxis,
};

TextCropCenterProjection ResolveTextCropCenterProjection(const wpscene::WPTextObject& object) {
    if (TextLayerUsesTightTransparentGlyphBounds(object) &&
        TextObjectUsesParagraphCenterPivot(object) && TextObjectUsesAutoSizedParagraphBox(object)) {
        return TextCropCenterProjection::ParentCrossAxis;
    }
    return TextCropCenterProjection::LocalLayoutBox;
}

Eigen::Matrix3f BuildTextPlacementRotation(const wpscene::WPTextObject& object) {
    Eigen::Affine3f transform = Eigen::Affine3f::Identity();
    transform.prerotate(Eigen::AngleAxisf(object.angles[0], Eigen::Vector3f::UnitX()));
    transform.prerotate(Eigen::AngleAxisf(object.angles[1], Eigen::Vector3f::UnitY()));
    transform.prerotate(Eigen::AngleAxisf(object.angles[2], Eigen::Vector3f::UnitZ()));
    return transform.linear();
}

Eigen::Matrix3f BuildTextPlacementLinearTransform(const wpscene::WPTextObject& object) {
    Eigen::Affine3f transform = Eigen::Affine3f::Identity();
    transform.prescale(Eigen::Vector3f { object.scale[0], object.scale[1], object.scale[2] });
    transform.prerotate(Eigen::AngleAxisf(object.angles[0], Eigen::Vector3f::UnitX()));
    transform.prerotate(Eigen::AngleAxisf(object.angles[1], Eigen::Vector3f::UnitY()));
    transform.prerotate(Eigen::AngleAxisf(object.angles[2], Eigen::Vector3f::UnitZ()));
    return transform.linear();
}

std::optional<Eigen::Vector3f> ResolveTextLocalOffsetForParentOffset(
    const wpscene::WPTextObject& object,
    const Eigen::Vector3f&       parent_offset) {
    const auto linear = BuildTextPlacementLinearTransform(object);
    if (std::abs(linear.determinant()) <= kTextPlacementEpsilon) return std::nullopt;
    return linear.inverse() * parent_offset;
}

Eigen::Vector3f OriginVector(std::array<float, 3> origin) {
    return Eigen::Vector3f { origin[0], origin[1], origin[2] };
}

float SignedScaleUnit(float value) {
    return value < 0.0f ? -1.0f : 1.0f;
}

std::array<float, 2> ResolveTextCropLocalCenter(
    const wpscene::WPTextObject& object,
    std::array<float, 3>         origin,
    std::array<float, 2>         crop_center) {
    if (ResolveTextCropCenterProjection(object) != TextCropCenterProjection::ParentCrossAxis ||
        std::abs(crop_center[0]) <= kTextPlacementEpsilon) {
        return crop_center;
    }

    Eigen::Vector3f parent_cross_direction =
        BuildTextPlacementRotation(object) * Eigen::Vector3f::UnitY();
    const float direction_length = parent_cross_direction.norm();
    if (direction_length <= kTextPlacementEpsilon) return crop_center;
    parent_cross_direction /= direction_length;

    const float origin_cross = OriginVector(origin).dot(parent_cross_direction);
    if (std::abs(origin_cross) <= kTextPlacementEpsilon) return crop_center;

    // Auto-sized transparent text can expose a tight cropped glyph rectangle while the scene node
    // still places the authored paragraph box. For an implicit paragraph-center pivot, inline crop
    // bias and the matching cross-axis correction must be projected through the same scale/rotation
    // transform that SceneNode consumes. This keeps the rule data-driven: character shape only
    // contributes measured crop metrics, while placement is determined by paragraph alignment,
    // origin, scale, and angles.
    Eigen::Vector3f resolved_center {
        crop_center[0] * SignedScaleUnit(object.scale[0]),
        0.0f,
        0.0f,
    };
    const Eigen::Vector3f parent_cross_offset =
        parent_cross_direction * -std::copysign(std::abs(crop_center[0]), origin_cross);
    const auto local_cross_offset =
        ResolveTextLocalOffsetForParentOffset(object, parent_cross_offset);
    if (!local_cross_offset.has_value()) return crop_center;

    resolved_center += *local_cross_offset;
    return {
        resolved_center.x(),
        resolved_center.y(),
    };
}

std::array<float, 2> ComputeCroppedDisplayOffset(std::string_view     alignment,
                                                 std::array<float, 2> full_display_size,
                                                 float crop_x, float crop_y, float crop_width,
                                                 float crop_height) {
    const std::array<float, 2> cropped_display_size {
        crop_width,
        crop_height,
    };
    const Eigen::Vector3f content_center_offset {
        -full_display_size[0] * 0.5f + crop_x + crop_width * 0.5f,
        full_display_size[1] * 0.5f - crop_y - crop_height * 0.5f,
        0.0f,
    };
    // Materialize the Eigen expression immediately. Using `auto` here stores an
    // unevaluated expression tree that can outlive the temporary vectors above
    // and produce garbage offsets nondeterministically.
    const Eigen::Vector3f applied_offset = AlignmentOffset(alignment, full_display_size) -
                                           AlignmentOffset(alignment, cropped_display_size) +
                                           content_center_offset;
    return {
        applied_offset.x(),
        applied_offset.y(),
    };
}

std::array<float, 2> ComputeCroppedContentCenter(std::array<float, 2> full_display_size,
                                                 float                crop_x,
                                                 float                crop_y,
                                                 float                crop_width,
                                                 float                crop_height) {
    // Glyph quads live inside one canonical text primitive. Their local translation only needs the
    // cropped bitmap center relative to the logical text box because anchor/alignment stay on the
    // owning scene node instead of being split across helper layers.
    return {
        -full_display_size[0] * 0.5f + crop_x + crop_width * 0.5f,
        full_display_size[1] * 0.5f - crop_y - crop_height * 0.5f,
    };
}

std::array<float, 2> ResolveVisibleTextDisplaySize(const TextLayerRuntimeState& state) {
    // Once a text layer has been materialized, the live scene primitive is the only authoritative
    // source of visible geometry. Falling back to the authored object is only for deferred logical
    // layers that do not have a shaped primitive yet.
    if (state.primitive != nullptr) return state.primitive->layout.visible_display_size;
    return state.object.size;
}

std::array<float, 2> ResolveVisibleTextSourceSize(const TextLayerRuntimeState& state) {
    if (state.primitive != nullptr) return state.primitive->layout.visible_source_size;
    // Deferred text layers have no rasterized primitive yet, so there is no authoritative source
    // texture rectangle to report. Returning the authored box here keeps diagnostics and placeholder
    // alignment stable until the primitive is materialized.
    return state.object.size;
}

std::array<float, 2> ResolveEffectDependencyTextSourceSize(const TextLayerRuntimeState& state) {
    // Authored text effects were historically sized against the same cropped glyph bitmap that
    // the scene exposed as the text layer's visible source. Switching the dependency contract to
    // the logical text box broke that invariant: the effect render targets stopped matching the
    // actual source pass, which is what produced the distorted weekday effect and the incorrect
    // offscreen growth decisions visible in the runtime log. Keep the dependency size aligned with
    // the visible glyph source, and solve update churn through node-capacity reuse rather than by
    // silently changing what "text source size" means to the effect system.
    return ResolveVisibleTextSourceSize(state);
}

bool GrowTextDependencyRenderTarget(SceneRenderTarget& render_target,
                                    std::array<float, 2> desired_source_size) {
    // A formal text source bridge always renders into an exact-size offscreen texture. Unlike the
    // old grow-only bucket path, the physical Vulkan target and the logical text source rectangle
    // are intentionally identical, so later effect shaders never need text-specific UV or
    // resolution compensation to recover the authored content width.
    const int32_t next_width =
        std::max(1, static_cast<int32_t>(std::lround(std::max(1.0f, desired_source_size[0]))));
    const int32_t next_height =
        std::max(1, static_cast<int32_t>(std::lround(std::max(1.0f, desired_source_size[1]))));
    const bool changed = next_width != render_target.width || next_height != render_target.height ||
                         next_width != render_target.mapWidth ||
                         next_height != render_target.mapHeight;
    render_target.width = next_width;
    render_target.height = next_height;
    render_target.mapWidth = next_width;
    render_target.mapHeight = next_height;
    return changed;
}

std::array<float, 2> ResolveTextBridgeRenderTargetSourceSize(
    const TextBridgeRenderTarget& bridge_target, std::array<float, 2> dependency_source_size) {
    const float source_width  = std::max(1.0f, dependency_source_size[0]);
    const float source_height = std::max(1.0f, dependency_source_size[1]);

    if (bridge_target.fit > 0) {
        const float longest_edge = std::max(source_width, source_height);
        const float fit_scale    = static_cast<float>(bridge_target.fit) / longest_edge;
        return {
            std::max(1.0f, source_width * fit_scale),
            std::max(1.0f, source_height * fit_scale),
        };
    }

    const float scale = static_cast<float>(std::max<uint32_t>(1u, bridge_target.scale));
    return {
        std::max(1.0f, source_width / scale),
        std::max(1.0f, source_height / scale),
    };
}

std::array<float, 2> ResolveFullTextDisplaySize(const TextLayerRuntimeState& state) {
    if (state.primitive == nullptr) {
        return state.object.size;
    }

    const auto& layout = state.primitive->layout;
    if (layout.logical_source_size[0] <= 0.0f || layout.logical_source_size[1] <= 0.0f ||
        layout.glyph_source_size[0] <= 0.0f || layout.glyph_source_size[1] <= 0.0f ||
        layout.glyph_display_size[0] <= 0.0f || layout.glyph_display_size[1] <= 0.0f) {
        return state.object.size;
    }

    const float display_scale_x = layout.glyph_display_size[0] / layout.glyph_source_size[0];
    const float display_scale_y = layout.glyph_display_size[1] / layout.glyph_source_size[1];
    return {
        layout.logical_source_size[0] * display_scale_x,
        layout.logical_source_size[1] * display_scale_y,
    };
}

std::array<float, 2> ResolveDerivedTextDisplayOffset(const TextLayerRuntimeState& state,
                                                     std::string_view             alignment) {
    if (state.object.opaquebackground || state.primitive == nullptr) {
        return { 0.0f, 0.0f };
    }

    const auto& layout = state.primitive->layout;
    if (layout.glyph_display_size[0] <= 0.0f || layout.glyph_display_size[1] <= 0.0f) {
        return { 0.0f, 0.0f };
    }

    const auto full_display_size = ResolveFullTextDisplaySize(state);
    const auto cropped_display_size = layout.glyph_display_size;
    const float crop_x = layout.glyph_offset[0] + full_display_size[0] * 0.5f -
                         cropped_display_size[0] * 0.5f;
    const float crop_y = full_display_size[1] * 0.5f - cropped_display_size[1] * 0.5f -
                         layout.glyph_offset[1];
    return ComputeCroppedDisplayOffset(alignment,
                                       full_display_size,
                                       crop_x,
                                       crop_y,
                                       cropped_display_size[0],
                                       cropped_display_size[1]);
}

std::array<float, 2> ResolveVisibleTextDisplayOffset(const TextLayerRuntimeState& state,
                                                     std::string_view             alignment) {
    if (state.object.opaquebackground) return { 0.0f, 0.0f };
    return ResolveDerivedTextDisplayOffset(state, alignment);
}

int ResolvePadding(const wpscene::WPTextObject& object) {
    return MaxTextPaddingEdge(ClampTextPaddingEdges(object.padding_edges));
}

float ResolveTextVisualScaleFactor(const wpscene::WPTextObject& object) {
    return std::max(
        { std::abs(object.scale[0]), std::abs(object.scale[1]), kMinTextVisualScaleFactor });
}

float ResolveTextRasterDensityFactor(const wpscene::WPTextObject& object) {
    const float visual_scale = ResolveTextVisualScaleFactor(object);
    // Effect-backed text always goes through at least one extra filtered pass,
    // so dropping its backing texture below 1:1 screen density makes it visibly
    // softer than plain text. Keep the existing sqrt() heuristic for larger
    // scales, but clamp effect layers to full density so we preserve sharpness
    // without changing any display-size math.
    if (! object.effects.empty()) return std::max(1.0f, std::sqrt(visual_scale));
    return std::sqrt(visual_scale);
}

std::array<float, 2> EstimateTextSourceSize(std::array<float, 2>         quad_size,
                                            const wpscene::WPTextObject& object,
                                            double                       render_scale) {
    (void)render_scale;
    const float raster_density = ResolveTextRasterDensityFactor(object);
    return {
        std::max(1.0f, static_cast<float>(std::lround(quad_size[0] * raster_density))),
        std::max(1.0f, static_cast<float>(std::lround(quad_size[1] * raster_density))),
    };
}

bool PropertyHasScriptOrAnimation(const nlohmann::json& json, const char* name) {
    if (! json.contains(name)) return false;
    const auto& value = json.at(name);
    return value.is_object() &&
           ((value.contains("script") && ! value.at("script").is_null()) ||
            (value.contains("animation") && ! value.at("animation").is_null()));
}

std::optional<std::string> ResolveFontFamily(fs::VFS& vfs, const std::string& font) {
    if (font.empty()) {
        return std::string("Sans");
    }
    if (! IsSupportedFontAssetPath(font)) {
        return NormalizeSystemFontAlias(font);
    }

    const auto asset_path      = NormalizeAssetPath(vfs, font);
    const auto vfs_identity    = vfs.Identity();
    const auto scene_cache_key = MakeSceneAssetFontCacheKey(vfs_identity, font, asset_path);
    auto&      cache_state     = GetAssetFontCacheState();

    {
        std::scoped_lock lock(cache_state.mutex);
        if (cache_state.missing_scene_asset_keys.count(scene_cache_key) != 0) {
            return std::nullopt;
        }
        if (const auto scene_it = cache_state.scene_asset_content_keys.find(scene_cache_key);
            scene_it != cache_state.scene_asset_content_keys.end()) {
            if (const auto entry_it = cache_state.entries.find(scene_it->second);
                entry_it != cache_state.entries.end()) {
                // The same scene can reraster animated text many times per second. Once a font has
                // been resolved for this VFS, return through the Pango visibility bridge directly
                // instead of re-opening and hashing large font files on every scripted text update.
                if (EnsureAssetFontVisibleToCurrentPangoFontMap(cache_state, entry_it->second)) {
                    return entry_it->second.family;
                }

                LOG_ERROR("rebuilding scene text font entry after visibility failure: request=%s asset=%s temp=%s hash=%s bytes=%zu",
                          entry_it->second.request_path.c_str(),
                          entry_it->second.asset_path.c_str(),
                          entry_it->second.temp_font_path.c_str(),
                          entry_it->second.content_hash.c_str(),
                          entry_it->second.byte_count);
                cache_state.entries.erase(entry_it);
            }
            cache_state.scene_asset_content_keys.erase(scene_it);
        }
    }

    auto       stream             = vfs.Open(asset_path);
    if (! stream) {
        std::scoped_lock lock(cache_state.mutex);
        cache_state.missing_scene_asset_keys.insert(scene_cache_key);
        LOG_ERROR("text layer font asset not found: %s", font.c_str());
        return std::nullopt;
    }

    std::string bytes = stream->ReadAllStr();
    if (bytes.empty()) {
        std::scoped_lock lock(cache_state.mutex);
        cache_state.missing_scene_asset_keys.insert(scene_cache_key);
        LOG_ERROR("text layer font asset is empty: %s", font.c_str());
        return std::nullopt;
    }

    const auto content_hash = utils::genSha1(std::span<const char>(bytes.data(), bytes.size()));
    const auto byte_count   = bytes.size();
    const auto cache_key    = MakeAssetFontCacheKey(font, asset_path, content_hash, byte_count);

    {
        std::scoped_lock lock(cache_state.mutex);
        if (const auto it = cache_state.entries.find(cache_key); it != cache_state.entries.end()) {
            cache_state.scene_asset_content_keys[scene_cache_key] = cache_key;
            // A content-cache hit only proves that another scene already parsed these same bytes and
            // generated the temp file. It does not prove that the PangoFcFontMap owned by this thread
            // has refreshed after that app-font registration, so every hit still runs the visibility
            // bridge before returning.
            if (EnsureAssetFontVisibleToCurrentPangoFontMap(cache_state, it->second)) {
                return it->second.family;
            }

            LOG_ERROR("rebuilding cached text font entry after visibility failure: request=%s asset=%s temp=%s hash=%s bytes=%zu",
                      it->second.request_path.c_str(),
                      it->second.asset_path.c_str(),
                      it->second.temp_font_path.c_str(),
                      it->second.content_hash.c_str(),
                      it->second.byte_count);
            cache_state.entries.erase(it);
            cache_state.scene_asset_content_keys.erase(scene_cache_key);
        }
    }

    FT_Library library { nullptr };
    if (FT_Init_FreeType(&library) != 0) {
        LOG_ERROR("FreeType init failed while loading text font: %s", font.c_str());
        return std::nullopt;
    }

    FT_Face face { nullptr };
    if (FT_New_Memory_Face(library,
                           reinterpret_cast<const FT_Byte*>(bytes.data()),
                           static_cast<FT_Long>(bytes.size()),
                           0,
                           &face) != 0) {
        FT_Done_FreeType(library);
        std::scoped_lock lock(cache_state.mutex);
        cache_state.missing_scene_asset_keys.insert(scene_cache_key);
        LOG_ERROR("FreeType failed to load text font: %s", font.c_str());
        return std::nullopt;
    }

    std::string family = face->family_name != nullptr ? face->family_name : "";
    if (family.empty()) {
        family = std::filesystem::path(font).stem().string();
    }

    FT_Done_Face(face);
    FT_Done_FreeType(library);

    const auto ext = std::filesystem::path(font).extension().string();
    AssetFontCacheEntry entry {
        .request_path = font,
        .asset_path = asset_path,
        .family = family,
        .temp_font_path =
            (std::filesystem::path(kFontCacheDir.data()) / (content_hash + ext)).string(),
        .content_hash = content_hash,
        .byte_count = byte_count,
    };

    // Asset fonts become regular temporary files once parsed because Fontconfig/Pango can only make
    // file-backed app fonts visible to layout engines. The cache stores the authored request, the VFS
    // asset path, and the content identity so later scene reuse can hit the cache without inventing a
    // separate per-wallpaper font namespace.
    std::scoped_lock lock(cache_state.mutex);
    if (! WriteAssetFontTempFile(entry, bytes)) {
        return std::nullopt;
    }
    if (! EnsureAssetFontVisibleToCurrentPangoFontMap(cache_state, entry)) {
        return std::nullopt;
    }

    cache_state.entries.emplace(cache_key, entry);
    cache_state.scene_asset_content_keys[scene_cache_key] = cache_key;
    return entry.family;
}

void ConfigureLayout(PangoLayout* layout, const wpscene::WPTextObject& object, int content_width) {
    if (layout == nullptr) return;

    pango_layout_set_text(layout, object.text.c_str(), -1);
    pango_layout_set_alignment(layout, ToPangoAlignment(object.horizontalalign));
    pango_layout_set_justify(layout, object.blockalign ? TRUE : FALSE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

    if (content_width > 0) {
        pango_layout_set_width(layout, content_width * PANGO_SCALE);
    } else {
        pango_layout_set_width(layout, -1);
    }

    if (object.limitrows) {
        pango_layout_set_height(layout, -std::max(object.maxrows, 1));
    } else {
        pango_layout_set_height(layout, -1);
    }

    if (object.limituseellipsis || object.limitrows || object.limitwidth) {
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    } else {
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    }
}

struct TextSurfaceCrop {
    int  x { 0 };
    int  y { 0 };
    int  width { 0 };
    int  height { 0 };
    int  margin { 0 };
    bool applied { false };
};

TextSurfaceCrop ResolveTextSurfaceCrop(const wpscene::WPTextObject& object, int raster_width,
                                       int raster_height, double raster_scale, int draw_x,
                                       int draw_y, const PangoRectangle& ink_rect,
                                       int crop_padding) {
    TextSurfaceCrop crop {
        .x       = 0,
        .y       = 0,
        .width   = raster_width,
        .height  = raster_height,
        .margin  = 0,
        .applied = false,
    };
    if (raster_width <= 0 || raster_height <= 0) {
        return crop;
    }
    if (object.has_dynamic_layout_script && object.effects.empty()) {
        return crop;
    }

    if (ink_rect.width <= 0 || ink_rect.height <= 0) return crop;

    const double logical_min_x = static_cast<double>(draw_x + ink_rect.x);
    const double logical_min_y = static_cast<double>(draw_y + ink_rect.y);
    const double logical_max_x = static_cast<double>(draw_x + ink_rect.x + ink_rect.width);
    const double logical_max_y = static_cast<double>(draw_y + ink_rect.y + ink_rect.height);

    int min_x = static_cast<int>(std::floor(logical_min_x * raster_scale));
    int min_y = static_cast<int>(std::floor(logical_min_y * raster_scale));
    int max_x = static_cast<int>(std::ceil(logical_max_x * raster_scale)) - 1;
    int max_y = static_cast<int>(std::ceil(logical_max_y * raster_scale)) - 1;

    const int effect_margin =
        object.effects.empty() ? 0 : std::max(1, static_cast<int>(std::ceil(raster_scale)));
    crop.margin = std::max({ static_cast<int>(std::lround(std::max(crop_padding, 0) * raster_scale)),
                             effect_margin,
                             std::max(1, static_cast<int>(std::ceil(raster_scale))) });

    min_x = std::max(0, min_x - crop.margin);
    min_y = std::max(0, min_y - crop.margin);
    max_x = std::min(raster_width - 1, max_x + crop.margin);
    max_y = std::min(raster_height - 1, max_y + crop.margin);

    crop.x       = min_x;
    crop.y       = min_y;
    crop.width   = std::max(1, max_x - min_x + 1);
    crop.height  = std::max(1, max_y - min_y + 1);
    crop.applied = crop.width < raster_width || crop.height < raster_height;
    return crop;
}

struct TextGlyphSourceBounds {
    float min_x { 0.0f };
    float min_y { 0.0f };
    float max_x { 0.0f };
    float max_y { 0.0f };
    bool  valid { false };
};

TextGlyphSourceBounds ResolveGlyphQuadSourceBounds(
    const std::vector<TextRasterLayoutResult::GlyphQuad>& quads) {
    TextGlyphSourceBounds bounds;
    bounds.min_x = std::numeric_limits<float>::max();
    bounds.min_y = std::numeric_limits<float>::max();
    bounds.max_x = std::numeric_limits<float>::lowest();
    bounds.max_y = std::numeric_limits<float>::lowest();

    for (const auto& quad : quads) {
        const float x = quad.source_rect[0];
        const float y = quad.source_rect[1];
        const float width = quad.source_rect[2];
        const float height = quad.source_rect[3];
        if (width <= 0.0f || height <= 0.0f) continue;
        if (!std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(width) || !std::isfinite(height)) {
            continue;
        }

        bounds.min_x = std::min(bounds.min_x, x);
        bounds.min_y = std::min(bounds.min_y, y);
        bounds.max_x = std::max(bounds.max_x, x + width);
        bounds.max_y = std::max(bounds.max_y, y + height);
        bounds.valid = true;
    }

    if (!bounds.valid) {
        bounds.min_x = bounds.min_y = bounds.max_x = bounds.max_y = 0.0f;
    }
    return bounds;
}

void NormalizeGlyphQuadSourceBounds(std::vector<TextRasterLayoutResult::GlyphQuad>& quads,
                                    const TextGlyphSourceBounds& bounds) {
    if (!bounds.valid) return;
    for (auto& quad : quads) {
        quad.source_rect[0] -= bounds.min_x;
        quad.source_rect[1] -= bounds.min_y;
    }
}

std::shared_ptr<Image> BuildImageFromRgbaPixels(const std::string&             texture_key,
                                                int                            width,
                                                int                            height,
                                                std::unique_ptr<uint8_t[]> rgba) {
    if (width <= 0 || height <= 0 || rgba == nullptr) return nullptr;

    auto image                     = std::make_shared<Image>();
    image->key                     = texture_key;
    image->header.width            = width;
    image->header.height           = height;
    image->header.mapWidth         = width;
    image->header.mapHeight        = height;
    image->header.count            = 1;
    image->header.format           = TextureFormat::RGBA8;
    image->header.type             = ImageType::PNG;
    image->header.sample.wrapS     = TextureWrap::CLAMP_TO_EDGE;
    image->header.sample.wrapT     = TextureWrap::CLAMP_TO_EDGE;
    image->header.sample.magFilter = TextureFilter::LINEAR;
    image->header.sample.minFilter = TextureFilter::LINEAR;

    image->slots.resize(1);
    image->slots[0].width  = width;
    image->slots[0].height = height;
    ImageData mipmap;
    mipmap.width  = width;
    mipmap.height = height;
    mipmap.size =
        static_cast<isize>(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    mipmap.data = ImageDataPtr(rgba.release(), [](uint8_t* ptr) {
        delete[] ptr;
    });
    image->slots[0].mipmaps.push_back(std::move(mipmap));
    return image;
}

struct TextGlyphBitmap {
    int                       width { 0 };
    int                       height { 0 };
    int                       origin_x_px { 0 };
    int                       origin_y_px { 0 };
    std::unique_ptr<uint8_t[]> rgba;
};

struct TextGlyphOccurrence {
    std::string cache_key;
    uint32_t    glyph_index { 0 };
    bool        cache_hit { false };
    int         origin_x_px { 0 };
    int         origin_y_px { 0 };
    float       x { 0.0f };
    float       y { 0.0f };
    int         width { 0 };
    int         height { 0 };
};

struct PackedTextGlyphEntry {
    uint32_t page_index { 0 };
    int      x { 0 };
    int      y { 0 };
    int      width { 0 };
    int      height { 0 };
};

struct TextGlyphAtlasBuildResult {
    std::vector<TextRasterLayoutResult::GlyphPage> pages;
    std::vector<TextRasterLayoutResult::GlyphQuad> quads;
    size_t                                          unique_glyph_count { 0 };
    size_t                                          glyph_instance_count { 0 };
    size_t                                          cache_hit_count { 0 };
    size_t                                          cache_miss_count { 0 };
};

std::string MakeTextGlyphCacheKey(PangoFont* font, PangoGlyph glyph, double raster_scale) {
    std::string description = "<unknown>";
    if (font != nullptr) {
        if (auto* desc = pango_font_describe(font); desc != nullptr) {
            description = pango_font_description_to_string(desc);
            pango_font_description_free(desc);
        }
    }

    std::ostringstream out;
    // Glyph rasters are cached by the concrete resolved font description, glyph index, and raster
    // scale so that repeated clock/script updates can reuse the same glyph coverage without
    // re-rasterizing the whole text layout or even the same glyph more than once. The explicit
    // atlas-raster version keeps long-lived renderer processes from reusing glyph bitmaps that
    // were generated under an older bounds contract or the pre-absolute-size font metric contract.
    out << description << "|glyph=" << glyph << "|scaleMilli="
        << static_cast<int>(std::lround(raster_scale * 1000.0)) << "|atlasRasterV=4";
    return out.str();
}

struct TextGlyphRasterBounds {
    double min_x { 0.0 };
    double min_y { 0.0 };
    double max_x { 0.0 };
    double max_y { 0.0 };
    int    min_x_px { 0 };
    int    min_y_px { 0 };
    int    max_x_px { 0 };
    int    max_y_px { 0 };
    int    width { 0 };
    int    height { 0 };
};

std::optional<TextGlyphRasterBounds> ResolveTextGlyphRasterBounds(PangoFont* font,
                                                                  PangoGlyph glyph,
                                                                  double     raster_scale) {
    if (font == nullptr || glyph == 0) return std::nullopt;

    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
    bool   have_bounds = false;

    // The atlas path must match the exact rasterizer that later draws the glyph coverage. Pango's
    // generic glyph ink rects are a good fallback, but stylized fonts can still render slightly
    // outside those bounds once Cairo applies the final hinting/scaled-font metrics. We therefore
    // query the Cairo scaled font first and only fall back to Pango extents when that bridge is
    // unavailable.
    if (PANGO_IS_CAIRO_FONT(font)) {
        if (auto* scaled_font = pango_cairo_font_get_scaled_font(PANGO_CAIRO_FONT(font));
            scaled_font != nullptr) {
            cairo_glyph_t cairo_glyph {};
            cairo_glyph.index = glyph;
            cairo_text_extents_t extents {};
            cairo_scaled_font_glyph_extents(scaled_font, &cairo_glyph, 1, &extents);
            if (extents.width > 0.0 && extents.height > 0.0) {
                min_x = extents.x_bearing;
                min_y = extents.y_bearing;
                max_x = extents.x_bearing + extents.width;
                max_y = extents.y_bearing + extents.height;
                have_bounds = true;
            }
        }
    }

    if (!have_bounds) {
        PangoRectangle ink_rect {};
        pango_font_get_glyph_extents(font, glyph, &ink_rect, nullptr);
        if (ink_rect.width <= 0 || ink_rect.height <= 0) return std::nullopt;

        min_x = static_cast<double>(ink_rect.x) / static_cast<double>(PANGO_SCALE);
        min_y = static_cast<double>(ink_rect.y) / static_cast<double>(PANGO_SCALE);
        max_x =
            static_cast<double>(ink_rect.x + ink_rect.width) / static_cast<double>(PANGO_SCALE);
        max_y =
            static_cast<double>(ink_rect.y + ink_rect.height) / static_cast<double>(PANGO_SCALE);
    }

    // The full-layout renderer always left a little raster-space slack around visible content.
    // Keeping the same gutter here prevents atlas glyphs from shaving off hinted edges or blur
    // fringes even when the text later expands into the larger runtime display geometry.
    const int glyph_margin = std::max(1, static_cast<int>(std::ceil(raster_scale)));
    TextGlyphRasterBounds bounds;
    bounds.min_x = min_x;
    bounds.min_y = min_y;
    bounds.max_x = max_x;
    bounds.max_y = max_y;
    bounds.min_x_px = static_cast<int>(std::floor(min_x * raster_scale)) - glyph_margin;
    bounds.min_y_px = static_cast<int>(std::floor(min_y * raster_scale)) - glyph_margin;
    bounds.max_x_px = static_cast<int>(std::ceil(max_x * raster_scale)) - 1 + glyph_margin;
    bounds.max_y_px = static_cast<int>(std::ceil(max_y * raster_scale)) - 1 + glyph_margin;
    bounds.width = std::max(1, bounds.max_x_px - bounds.min_x_px + 1);
    bounds.height = std::max(1, bounds.max_y_px - bounds.min_y_px + 1);
    return bounds;
}

struct TextGlyphCoverageBounds {
    int min_x { 0 };
    int min_y { 0 };
    int max_x { 0 };
    int max_y { 0 };
    bool has_coverage { false };
};

TextGlyphCoverageBounds ResolveTextGlyphCoverageBounds(const uint8_t* data,
                                                       int            width,
                                                       int            height,
                                                       int            stride) {
    TextGlyphCoverageBounds bounds;
    if (data == nullptr || width <= 0 || height <= 0 || stride <= 0) return bounds;

    for (int y = 0; y < height; y++) {
        const auto* row = data + stride * y;
        for (int x = 0; x < width; x++) {
            if (row[x * 4 + 3] == 0) continue;
            if (!bounds.has_coverage) {
                bounds.min_x = bounds.max_x = x;
                bounds.min_y = bounds.max_y = y;
                bounds.has_coverage = true;
                continue;
            }
            bounds.min_x = std::min(bounds.min_x, x);
            bounds.min_y = std::min(bounds.min_y, y);
            bounds.max_x = std::max(bounds.max_x, x);
            bounds.max_y = std::max(bounds.max_y, y);
        }
    }
    return bounds;
}

std::shared_ptr<TextGlyphBitmap> BuildTextGlyphBitmap(PangoFont*   font,
                                                      PangoGlyph   glyph,
                                                      double       raster_scale,
                                                      std::string* out_error) {
    if (font == nullptr || glyph == 0) return nullptr;

    const auto bounds = ResolveTextGlyphRasterBounds(font, glyph, raster_scale);
    if (!bounds.has_value()) return nullptr;

    // The atlas now derives its final bitmap bounds from actual rendered coverage instead of
    // trusting font-reported ink extents as the final truth. We first raster onto a scratch
    // surface with extra slack, then crop to the alpha bounds that Cairo really produced. This
    // makes the cached glyph bitmap and the scene-space placement use the same raster contract.
    const int scratch_slack = std::max(2, static_cast<int>(std::ceil(raster_scale)) * 2);
    const int scratch_min_x_px = bounds->min_x_px - scratch_slack;
    const int scratch_min_y_px = bounds->min_y_px - scratch_slack;
    const int scratch_max_x_px = bounds->max_x_px + scratch_slack;
    const int scratch_max_y_px = bounds->max_y_px + scratch_slack;
    const int scratch_width = std::max(1, scratch_max_x_px - scratch_min_x_px + 1);
    const int scratch_height = std::max(1, scratch_max_y_px - scratch_min_y_px + 1);

    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, scratch_width, scratch_height);
    cairo_surface_set_device_scale(surface, raster_scale, raster_scale);
    auto* cr = cairo_create(surface);
    ApplyTextCairoRenderOptions(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    auto* glyphs = pango_glyph_string_new();
    pango_glyph_string_set_size(glyphs, 1);
    glyphs->glyphs[0].glyph             = glyph;
    glyphs->glyphs[0].geometry.width    = 0;
    glyphs->glyphs[0].geometry.x_offset = 0;
    glyphs->glyphs[0].geometry.y_offset = 0;
    glyphs->glyphs[0].attr.is_cluster_start = 1;

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    // Raster each cached glyph into its own local origin. The atlas builder later places that
    // bitmap onto shared pages while the page mesh positions the glyph in scene space.
    cairo_move_to(cr,
                  -static_cast<double>(scratch_min_x_px) / raster_scale,
                  -static_cast<double>(scratch_min_y_px) / raster_scale);
    pango_cairo_show_glyph_string(cr, font, glyphs);

    cairo_surface_flush(surface);
    auto* data   = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const auto coverage = ResolveTextGlyphCoverageBounds(data, scratch_width, scratch_height, stride);
    if (!coverage.has_coverage) {
        pango_glyph_string_free(glyphs);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return nullptr;
    }

    // Keep one raster pixel of slack around the actual coverage so linear sampling and blur passes
    // do not immediately sample outside the cached glyph image.
    const int coverage_gutter = 1;
    const int crop_min_x = std::max(0, coverage.min_x - coverage_gutter);
    const int crop_min_y = std::max(0, coverage.min_y - coverage_gutter);
    const int crop_max_x = std::min(scratch_width - 1, coverage.max_x + coverage_gutter);
    const int crop_max_y = std::min(scratch_height - 1, coverage.max_y + coverage_gutter);
    const int width = std::max(1, crop_max_x - crop_min_x + 1);
    const int height = std::max(1, crop_max_y - crop_min_y + 1);

    auto rgba = std::unique_ptr<uint8_t[]>(
        new uint8_t[static_cast<size_t>(width) * static_cast<size_t>(height) * 4]);
    for (int y = 0; y < height; y++) {
        const auto* src_row = data + stride * (crop_min_y + y);
        auto*       dst_row =
            rgba.get() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (int x = 0; x < width; x++) {
            const auto* src = src_row + (crop_min_x + x) * 4;
            auto*       dst = dst_row + x * 4;
            const uint8_t b = src[0];
            const uint8_t g = src[1];
            const uint8_t r = src[2];
            const uint8_t a = src[3];

            if (a == 0) {
                dst[0] = dst[1] = dst[2] = dst[3] = 0;
                continue;
            }

            dst[0] = static_cast<uint8_t>(std::min(255, (static_cast<int>(r) * 255) / a));
            dst[1] = static_cast<uint8_t>(std::min(255, (static_cast<int>(g) * 255) / a));
            dst[2] = static_cast<uint8_t>(std::min(255, (static_cast<int>(b) * 255) / a));
            dst[3] = a;
        }
    }

    pango_glyph_string_free(glyphs);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    auto bitmap = std::make_shared<TextGlyphBitmap>();
    bitmap->width = width;
    bitmap->height = height;
    bitmap->origin_x_px = scratch_min_x_px + crop_min_x;
    bitmap->origin_y_px = scratch_min_y_px + crop_min_y;
    bitmap->rgba = std::move(rgba);
    (void)out_error;
    return bitmap;
}

std::shared_ptr<TextGlyphBitmap> GetOrCreateTextGlyphBitmap(PangoFont*   font,
                                                            PangoGlyph   glyph,
                                                            double       raster_scale,
                                                            bool*        out_cache_hit,
                                                            std::string* out_error) {
    static std::mutex                                                  cache_mutex;
    static std::unordered_map<std::string, std::shared_ptr<TextGlyphBitmap>> glyph_cache;

    const auto key = MakeTextGlyphCacheKey(font, glyph, raster_scale);
    {
        std::scoped_lock lock(cache_mutex);
        if (const auto it = glyph_cache.find(key); it != glyph_cache.end()) {
            if (out_cache_hit != nullptr) *out_cache_hit = true;
            return it->second;
        }
    }

    auto bitmap = BuildTextGlyphBitmap(font, glyph, raster_scale, out_error);
    if (bitmap == nullptr) return nullptr;

    std::scoped_lock lock(cache_mutex);
    auto [it, inserted] = glyph_cache.emplace(key, bitmap);
    if (out_cache_hit != nullptr) *out_cache_hit = !inserted;
    return it->second;
}

void CopyGlyphBitmapIntoAtlas(uint8_t*                 dst_rgba,
                              int                      dst_width,
                              int                      dst_height,
                              int                      dst_x,
                              int                      dst_y,
                              const TextGlyphBitmap& source) {
    if (dst_rgba == nullptr || source.rgba == nullptr) return;
    if (source.width <= 0 || source.height <= 0) return;

    auto copy_pixel = [&](int dst_px, int dst_py, int src_px, int src_py) {
        if (dst_px < 0 || dst_py < 0 || dst_px >= dst_width || dst_py >= dst_height) return;
        const auto src_index =
            (static_cast<size_t>(src_py) * static_cast<size_t>(source.width) +
             static_cast<size_t>(src_px)) *
            4;
        const auto dst_index =
            (static_cast<size_t>(dst_py) * static_cast<size_t>(dst_width) +
             static_cast<size_t>(dst_px)) *
            4;
        std::copy_n(source.rgba.get() + src_index, 4, dst_rgba + dst_index);
    };

    for (int y = 0; y < source.height; y++) {
        for (int x = 0; x < source.width; x++) {
            copy_pixel(dst_x + x, dst_y + y, x, y);
        }
    }

    // Duplicate the edge pixels around each packed glyph. The atlas still uses linear sampling via
    // `genericimage4`, so this one-pixel gutter prevents neighboring transparent texels from
    // bleeding into glyph edges when the text is scaled or blurred by image effects.
    for (int x = 0; x < source.width; x++) {
        copy_pixel(dst_x + x, dst_y - 1, x, 0);
        copy_pixel(dst_x + x, dst_y + source.height, x, source.height - 1);
    }
    for (int y = 0; y < source.height; y++) {
        copy_pixel(dst_x - 1, dst_y + y, 0, y);
        copy_pixel(dst_x + source.width, dst_y + y, source.width - 1, y);
    }
    copy_pixel(dst_x - 1, dst_y - 1, 0, 0);
    copy_pixel(dst_x + source.width, dst_y - 1, source.width - 1, 0);
    copy_pixel(dst_x - 1, dst_y + source.height, 0, source.height - 1);
    copy_pixel(dst_x + source.width,
               dst_y + source.height,
               source.width - 1,
               source.height - 1);
}

std::optional<TextGlyphAtlasBuildResult> BuildTextGlyphAtlas(
    PangoLayout*          layout,
    const std::string&    texture_key,
    double                raster_scale,
    const TextSurfaceCrop& crop,
    int                   draw_x,
    int                   draw_y,
    std::string*          out_error) {
    if (layout == nullptr) return TextGlyphAtlasBuildResult {};

    std::vector<TextGlyphOccurrence> occurrences;
    std::unordered_map<std::string, std::shared_ptr<TextGlyphBitmap>> unique_bitmaps;
    size_t cache_hits = 0;
    size_t cache_misses = 0;

    auto* iter = pango_layout_get_iter(layout);
    if (iter == nullptr) return TextGlyphAtlasBuildResult {};

    do {
        PangoLayoutRun* run = pango_layout_iter_get_run_readonly(iter);
        if (run == nullptr || run->glyphs == nullptr || run->glyphs->num_glyphs <= 0 ||
            run->item == nullptr || run->item->analysis.font == nullptr) {
            continue;
        }

        PangoRectangle run_logical_rect {};
        pango_layout_iter_get_run_extents(iter, nullptr, &run_logical_rect);
        const double run_x =
            static_cast<double>(run_logical_rect.x) / static_cast<double>(PANGO_SCALE);
        const double baseline =
            static_cast<double>(pango_layout_iter_get_baseline(iter)) /
            static_cast<double>(PANGO_SCALE);

        int glyph_advance_units = 0;
        for (int glyph_index = 0; glyph_index < run->glyphs->num_glyphs; glyph_index++) {
            const auto& glyph_info = run->glyphs->glyphs[glyph_index];
            const auto glyph = glyph_info.glyph;
            if (glyph == 0) {
                glyph_advance_units += glyph_info.geometry.width;
                continue;
            }

            bool cache_hit = false;
            const auto bitmap = GetOrCreateTextGlyphBitmap(
                run->item->analysis.font, glyph, raster_scale, &cache_hit, out_error);
            if (bitmap == nullptr) {
                glyph_advance_units += glyph_info.geometry.width;
                continue;
            }

            const double glyph_origin_x =
                static_cast<double>(draw_x) + run_x +
                static_cast<double>(glyph_advance_units + glyph_info.geometry.x_offset) /
                    static_cast<double>(PANGO_SCALE);
            const double glyph_origin_y =
                static_cast<double>(draw_y) + baseline -
                static_cast<double>(glyph_info.geometry.y_offset) /
                    static_cast<double>(PANGO_SCALE);
            // Preserve the layout engine's subpixel glyph origins all the way into the generated
            // source quads. The first atlas implementation rounded every glyph occurrence to an
            // integer source pixel independently, which let tiny spacing errors accumulate across
            // long strings such as `T U E S D A Y` and `21 APR 2026`. Those per-glyph rounding
            // losses do not show up as a dramatic total-size mismatch in the log, but they do make
            // certain fonts and spaced-out workshop layouts look visibly narrower than Wallpaper
            // Engine's original one-shot raster path. Keeping floating-point source positions here
            // matches the original layout contract much more closely while the atlas bitmaps
            // themselves remain integer-aligned inside their pages.
            const float rect_x =
                static_cast<float>(glyph_origin_x * raster_scale +
                                   static_cast<double>(bitmap->origin_x_px - crop.x));
            const float rect_y =
                static_cast<float>(glyph_origin_y * raster_scale +
                                   static_cast<double>(bitmap->origin_y_px - crop.y));

            if (cache_hit) {
                cache_hits++;
            } else {
                cache_misses++;
            }
            unique_bitmaps.emplace(MakeTextGlyphCacheKey(run->item->analysis.font, glyph, raster_scale),
                                   bitmap);
            occurrences.push_back(TextGlyphOccurrence {
                .cache_key = MakeTextGlyphCacheKey(run->item->analysis.font, glyph, raster_scale),
                .glyph_index = glyph,
                .cache_hit = cache_hit,
                .origin_x_px = bitmap->origin_x_px,
                .origin_y_px = bitmap->origin_y_px,
                .x = rect_x,
                .y = rect_y,
                .width = bitmap->width,
                .height = bitmap->height,
            });
            glyph_advance_units += glyph_info.geometry.width;
        }
    } while (pango_layout_iter_next_run(iter));
    pango_layout_iter_free(iter);

    TextGlyphAtlasBuildResult result;
    result.unique_glyph_count = unique_bitmaps.size();
    result.glyph_instance_count = occurrences.size();
    result.cache_hit_count = cache_hits;
    result.cache_miss_count = cache_misses;

    if (unique_bitmaps.empty()) {
        auto placeholder = CreateSceneScriptSolidImage(texture_key + "__glyph_page_0",
                                                       { 0, 0, 0, 0 });
        result.pages.push_back(TextRasterLayoutResult::GlyphPage {
            .image = placeholder,
            .source_size = { 1.0f, 1.0f },
        });
        return result;
    }

    struct AtlasPageBuffer {
        int                        width { 0 };
        int                        height { 0 };
        int                        cursor_x { kTextGlyphAtlasPadding };
        int                        cursor_y { kTextGlyphAtlasPadding };
        int                        row_height { 0 };
        int                        used_width { 0 };
        int                        used_height { 0 };
        std::unique_ptr<uint8_t[]> rgba;
    };

    const int max_bitmap_width =
        std::max_element(unique_bitmaps.begin(),
                         unique_bitmaps.end(),
                         [](const auto& lhs, const auto& rhs) {
                             return lhs.second->width < rhs.second->width;
                         })
            ->second->width +
        kTextGlyphAtlasPadding * 2;
    const int target_page_width = std::clamp(max_bitmap_width, 64, kTextGlyphAtlasMaxExtent);

    std::unordered_map<std::string, PackedTextGlyphEntry> packed_entries;
    std::vector<AtlasPageBuffer> atlas_pages;

    auto begin_page = [&]() -> AtlasPageBuffer& {
        AtlasPageBuffer page;
        page.width = target_page_width;
        page.height = kTextGlyphAtlasMaxExtent;
        page.rgba = std::unique_ptr<uint8_t[]>(
            new uint8_t[static_cast<size_t>(page.width) * static_cast<size_t>(page.height) * 4]);
        std::fill_n(page.rgba.get(),
                    static_cast<size_t>(page.width) * static_cast<size_t>(page.height) * 4,
                    0);
        atlas_pages.push_back(std::move(page));
        return atlas_pages.back();
    };

    auto* current_page = &begin_page();
    for (const auto& [cache_key, bitmap] : unique_bitmaps) {
        const int padded_width = bitmap->width + kTextGlyphAtlasPadding * 2;
        const int padded_height = bitmap->height + kTextGlyphAtlasPadding * 2;
        if (padded_width > kTextGlyphAtlasMaxExtent || padded_height > kTextGlyphAtlasMaxExtent) {
            if (out_error != nullptr) {
                *out_error = "text glyph atlas entry exceeds maximum extent";
            }
            return std::nullopt;
        }

        if (current_page->cursor_x + padded_width > current_page->width) {
            current_page->cursor_x = kTextGlyphAtlasPadding;
            current_page->cursor_y += current_page->row_height;
            current_page->row_height = 0;
        }
        if (current_page->cursor_y + padded_height > current_page->height) {
            current_page = &begin_page();
        }

        const int atlas_x = current_page->cursor_x + kTextGlyphAtlasPadding;
        const int atlas_y = current_page->cursor_y + kTextGlyphAtlasPadding;
        CopyGlyphBitmapIntoAtlas(current_page->rgba.get(),
                                 current_page->width,
                                 current_page->height,
                                 atlas_x,
                                 atlas_y,
                                 *bitmap);

        packed_entries.emplace(cache_key,
                               PackedTextGlyphEntry {
                                   .page_index =
                                       static_cast<uint32_t>(atlas_pages.size() - 1),
                                   .x = atlas_x,
                                   .y = atlas_y,
                                   .width = bitmap->width,
                                   .height = bitmap->height,
                               });

        current_page->cursor_x += padded_width;
        current_page->row_height = std::max(current_page->row_height, padded_height);
        current_page->used_width =
            std::max(current_page->used_width, atlas_x + bitmap->width + kTextGlyphAtlasPadding);
        current_page->used_height =
            std::max(current_page->used_height, atlas_y + bitmap->height + kTextGlyphAtlasPadding);
    }

    for (size_t page_index = 0; page_index < atlas_pages.size(); page_index++) {
        auto& page = atlas_pages[page_index];
        const int image_width = std::max(1, page.used_width);
        const int image_height = std::max(1, page.used_height);
        auto trimmed_rgba = std::unique_ptr<uint8_t[]>(
            new uint8_t[static_cast<size_t>(image_width) * static_cast<size_t>(image_height) * 4]);
        for (int y = 0; y < image_height; y++) {
            std::copy_n(page.rgba.get() + static_cast<size_t>(y) * static_cast<size_t>(page.width) * 4,
                        static_cast<size_t>(image_width) * 4,
                        trimmed_rgba.get() +
                            static_cast<size_t>(y) * static_cast<size_t>(image_width) * 4);
        }
        result.pages.push_back(TextRasterLayoutResult::GlyphPage {
            .image = BuildImageFromRgbaPixels(texture_key + "__glyph_page_" +
                                                  std::to_string(page_index),
                                              image_width,
                                              image_height,
                                              std::move(trimmed_rgba)),
            .source_size = {
                static_cast<float>(image_width),
                static_cast<float>(image_height),
            },
        });
    }

    for (const auto& occurrence : occurrences) {
        const auto packed_it = packed_entries.find(occurrence.cache_key);
        if (packed_it == packed_entries.end()) continue;

        result.quads.push_back(TextRasterLayoutResult::GlyphQuad {
            .page_index = packed_it->second.page_index,
            .source_rect = {
                static_cast<float>(occurrence.x),
                static_cast<float>(occurrence.y),
                static_cast<float>(occurrence.width),
                static_cast<float>(occurrence.height),
            },
            .atlas_rect = {
                static_cast<float>(packed_it->second.x),
                static_cast<float>(packed_it->second.y),
                static_cast<float>(packed_it->second.width),
                static_cast<float>(packed_it->second.height),
            },
        });
    }

    return result;
}

bool GenerateTextLayoutImage(fs::VFS& vfs, wpscene::WPTextObject& object,
                             const std::string& texture_key, double render_scale,
                             TextRasterLayoutResult* out_image, std::string* out_error) {
    if (out_image == nullptr) return false;

    const auto   input_size          = object.size;
    const bool   input_size_explicit = object.size_explicit;
    const auto   font_family      = ResolveFontFamily(vfs, object.font).value_or("Sans");
    const auto   authored_padding = ResolveTextLayoutPadding(object);
    const double scene_scale    = std::max(1.0, render_scale);
    const float  raster_density = ResolveTextRasterDensityFactor(object);
    // Text layout has two deliberately different scale contracts. Glyphs are still shaped at the
    // authored `pointsize` and then rasterized into a denser backing atlas, because that preserves
    // the Wallpaper Engine font size that users see. The authored text box (`size`, `maxwidth`,
    // and `padding`) is different: it is a display-space rectangle, so it must be converted down to
    // the Pango layout coordinate space before wrapping is computed. Keeping these paths separate
    // fixes HiDPI/effect text wrapping without shrinking the glyph quads themselves.
    const double raster_scale =
        std::max(static_cast<double>(kMinTextVisualScaleFactor),
                 scene_scale * scene_scale * static_cast<double>(raster_density));
    const double display_scale =
        1.0 / static_cast<double>(std::max(raster_density, kMinTextVisualScaleFactor));
    const double display_units_per_layout_unit = raster_scale * display_scale;
    auto scale_display_metric_to_layout_pixels = [&](double value, int minimum) {
        if (value <= 0.0) return minimum;
        return std::max(minimum,
                        static_cast<int>(
                            std::lround(value / std::max(display_units_per_layout_unit,
                                                          static_cast<double>(kMinTextVisualScaleFactor)))));
    };
    std::array<int32_t, 4> padding {
        scale_display_metric_to_layout_pixels(static_cast<double>(authored_padding[0]),
                                              authored_padding[0] > 0 ? 1 : 0),
        scale_display_metric_to_layout_pixels(static_cast<double>(authored_padding[1]),
                                              authored_padding[1] > 0 ? 1 : 0),
        scale_display_metric_to_layout_pixels(static_cast<double>(authored_padding[2]),
                                              authored_padding[2] > 0 ? 1 : 0),
        scale_display_metric_to_layout_pixels(static_cast<double>(authored_padding[3]),
                                              authored_padding[3] > 0 ? 1 : 0),
    };
    const int padding_horizontal = TextPaddingHorizontal(padding);
    const int padding_vertical   = TextPaddingVertical(padding);

    auto* measure_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    auto* measure_cr      = cairo_create(measure_surface);
    auto* measure_layout  = pango_cairo_create_layout(measure_cr);
    auto* desc            = pango_font_description_new();
    ApplyTextFontRenderOptions(measure_cr, measure_layout);
    ApplyTextResolution(measure_layout);

    pango_font_description_set_family(desc, font_family.c_str());
    const double effective_point_size = std::max(1.0, static_cast<double>(object.pointsize));
    ApplyWallpaperEngineTextSize(desc, effective_point_size);
    pango_layout_set_font_description(measure_layout, desc);

    const int requested_max_width =
        scale_display_metric_to_layout_pixels(static_cast<double>(object.maxwidth), 0);
    const int measure_width =
        object.limitwidth ? std::max(0, requested_max_width - padding_horizontal) : -1;
    ConfigureLayout(measure_layout, object, measure_width);

    int            layout_width  = 0;
    int            layout_height = 0;
    PangoRectangle ink_rect {};
    PangoRectangle logical_rect {};
    pango_layout_get_pixel_size(measure_layout, &layout_width, &layout_height);
    pango_layout_get_pixel_extents(measure_layout, &ink_rect, &logical_rect);
    const int            ink_overhang_left     = std::max(-ink_rect.x, 0);
    const int            ink_overhang_top      = std::max(-ink_rect.y, 0);
    const int ink_overhang_right  = std::max((ink_rect.x + ink_rect.width) - layout_width, 0);
    const int ink_overhang_bottom = std::max((ink_rect.y + ink_rect.height) - layout_height, 0);
    const int bounds_width  = std::max(layout_width + ink_overhang_left + ink_overhang_right, 1);
    const int bounds_height = std::max(layout_height + ink_overhang_top + ink_overhang_bottom, 1);

    int resolved_width =
        scale_display_metric_to_layout_pixels(static_cast<double>(object.size[0]), 1);
    int resolved_height =
        scale_display_metric_to_layout_pixels(static_cast<double>(object.size[1]), 1);
    if (! object.size_explicit || object.size[0] <= 0.0f || object.size[1] <= 0.0f) {
        resolved_width =
            requested_max_width > 0 ? requested_max_width
                                    : std::max(bounds_width + padding_horizontal, 1);
        resolved_height = std::max(bounds_height + padding_vertical, 1);
        object.size     = {
            static_cast<float>(static_cast<double>(resolved_width) *
                               display_units_per_layout_unit),
            static_cast<float>(static_cast<double>(resolved_height) *
                               display_units_per_layout_unit),
        };
    } else if (! object.limitwidth) {
        // Non-width-limited text should never be clipped just because Linux font
        // metrics are a few pixels wider than the original authoring environment.
        resolved_width  = std::max(resolved_width, bounds_width + padding_horizontal);
        resolved_height = std::max(resolved_height, bounds_height + padding_vertical);
        object.size     = {
            static_cast<float>(static_cast<double>(resolved_width) *
                               display_units_per_layout_unit),
            static_cast<float>(static_cast<double>(resolved_height) *
                               display_units_per_layout_unit),
        };
    }

    const int width  = resolved_width;
    const int height = resolved_height;
    const int raster_width =
        std::max(1, static_cast<int>(std::lround(static_cast<double>(width) * raster_scale)));
    const int raster_height =
        std::max(1, static_cast<int>(std::lround(static_cast<double>(height) * raster_scale)));

    g_object_unref(measure_layout);
    pango_font_description_free(desc);
    cairo_destroy(measure_cr);
    cairo_surface_destroy(measure_surface);

    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    auto* cr        = cairo_create(surface);
    auto* layout    = pango_cairo_create_layout(cr);
    auto* draw_desc = pango_font_description_new();
    ApplyTextFontRenderOptions(cr, layout);
    ApplyTextResolution(layout);
    pango_font_description_set_family(draw_desc, font_family.c_str());
    ApplyWallpaperEngineTextSize(draw_desc, effective_point_size);
    pango_layout_set_font_description(layout, draw_desc);

    const int content_width      = std::max(width - padding_horizontal, 1);
    const int draw_content_width = object.limitwidth ? content_width : -1;
    ConfigureLayout(layout, object, draw_content_width);
    pango_layout_get_pixel_size(layout, &layout_width, &layout_height);
    pango_layout_get_pixel_extents(layout, &ink_rect, &logical_rect);
    const int            draw_overhang_left = std::max(-ink_rect.x, 0);
    const int            draw_overhang_top  = std::max(-ink_rect.y, 0);
    const int draw_overhang_right  = std::max((ink_rect.x + ink_rect.width) - layout_width, 0);
    const int draw_overhang_bottom = std::max((ink_rect.y + ink_rect.height) - layout_height, 0);
    const int draw_bounds_width =
        std::max(layout_width + draw_overhang_left + draw_overhang_right, 1);
    const int draw_bounds_height =
        std::max(layout_height + draw_overhang_top + draw_overhang_bottom, 1);

    auto clamp_int = [](int value, int low, int high) {
        return std::min(std::max(value, low), high);
    };

    int draw_x = padding[3] + draw_overhang_left;
    int draw_y = padding[0] + draw_overhang_top;
    if (! object.limitwidth) {
        const int min_draw_x = draw_overhang_left;
        const int max_draw_x = std::max(min_draw_x, width - draw_bounds_width + draw_overhang_left);
        int       preferred_draw_x = padding[3] + draw_overhang_left;
        if (object.horizontalalign == "center") {
            preferred_draw_x = (width - draw_bounds_width) / 2 + draw_overhang_left;
        } else if (object.horizontalalign == "right") {
            preferred_draw_x = width - padding[1] - draw_bounds_width + draw_overhang_left;
        }
        draw_x = clamp_int(preferred_draw_x, min_draw_x, max_draw_x);
    }
    {
        const int min_draw_y = draw_overhang_top;
        const int max_draw_y =
            std::max(min_draw_y, height - draw_bounds_height + draw_overhang_top);
        int preferred_draw_y = padding[0] + draw_overhang_top;
        if (object.verticalalign == "center") {
            preferred_draw_y = (height - draw_bounds_height) / 2 + draw_overhang_top;
        } else if (object.verticalalign == "bottom") {
            preferred_draw_y = height - padding[2] - draw_bounds_height + draw_overhang_top;
        }
        draw_y = clamp_int(preferred_draw_y, min_draw_y, max_draw_y);
    }

    const auto crop = ResolveTextSurfaceCrop(
        object,
        raster_width,
        raster_height,
        raster_scale,
        draw_x,
        draw_y,
        ink_rect,
        MaxTextPaddingEdge(padding));
    const std::array<float, 2> full_display_size {
        static_cast<float>(static_cast<double>(raster_width) * static_cast<double>(display_scale)),
        static_cast<float>(static_cast<double>(raster_height) * static_cast<double>(display_scale)),
    };

    auto atlas_result = BuildTextGlyphAtlas(
        layout, texture_key, raster_scale, crop, draw_x, draw_y, out_error);
    if (!atlas_result.has_value()) {
        if (out_error != nullptr && out_error->empty()) {
            *out_error = "failed to build glyph atlas pages";
        }
        g_object_unref(layout);
        pango_font_description_free(draw_desc);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return false;
    }

    float visible_source_x = static_cast<float>(crop.x);
    float visible_source_y = static_cast<float>(crop.y);
    float visible_source_width = static_cast<float>(crop.width);
    float visible_source_height = static_cast<float>(crop.height);
    const bool normalized_direct_text = TextLayerUsesTightTransparentGlyphBounds(object);
    if (normalized_direct_text) {
        const auto glyph_bounds = ResolveGlyphQuadSourceBounds(atlas_result->quads);
        if (glyph_bounds.valid) {
            NormalizeGlyphQuadSourceBounds(atlas_result->quads, glyph_bounds);
            visible_source_x += glyph_bounds.min_x;
            visible_source_y += glyph_bounds.min_y;
            visible_source_width = std::max(1.0f, glyph_bounds.max_x - glyph_bounds.min_x);
            visible_source_height = std::max(1.0f, glyph_bounds.max_y - glyph_bounds.min_y);
        }
    }

    const std::array<float, 2> cropped_display_size {
        static_cast<float>(static_cast<double>(visible_source_width) *
                           static_cast<double>(display_scale)),
        static_cast<float>(static_cast<double>(visible_source_height) *
                           static_cast<double>(display_scale)),
    };
    const auto scene_alignment = ResolveTextLayerSceneAlignment(object);
    const auto display_offset =
        ComputeCroppedDisplayOffset(scene_alignment,
                                    full_display_size,
                                    visible_source_x * static_cast<float>(display_scale),
                                    visible_source_y * static_cast<float>(display_scale),
                                    cropped_display_size[0],
                                    cropped_display_size[1]);
    const auto glyph_offset =
        ComputeCroppedContentCenter(full_display_size,
                                    visible_source_x * static_cast<float>(display_scale),
                                    visible_source_y * static_cast<float>(display_scale),
                                    cropped_display_size[0],
                                    cropped_display_size[1]);

    out_image->logical_size = full_display_size;
    out_image->logical_source_size = {
        static_cast<float>(raster_width),
        static_cast<float>(raster_height),
    };
    out_image->glyph_display_size = cropped_display_size;
    out_image->glyph_source_size  = {
        visible_source_width,
        visible_source_height,
    };
    out_image->glyph_offset   = glyph_offset;
    out_image->display_offset = display_offset;
    out_image->glyph_pages = atlas_result->pages;
    out_image->glyph_quads = atlas_result->quads;

    g_object_unref(layout);
    pango_font_description_free(draw_desc);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return true;
}

Eigen::Vector3f AlignmentOffset(std::string_view alignment, std::array<float, 2> size) {
    const float half_width  = size[0] * 0.5f;
    const float half_height = size[1] * 0.5f;
    const auto  contains    = [&](std::string_view value) {
        return alignment.find(value) != std::string_view::npos;
    };

    Eigen::Vector3f offset { 0.0f, 0.0f, 0.0f };
    if (contains("top")) offset.y() -= half_height;
    if (contains("left")) offset.x() += half_width;
    if (contains("right")) offset.x() -= half_width;
    if (contains("bottom")) offset.y() += half_height;
    return offset;
}

std::array<float, 2> ResolveTextPlacementDisplaySize(const TextLayerRuntimeState& state) {
    if (state.primitive != nullptr) return state.primitive->layout.logical_size;
    return state.object.size;
}

std::array<float, 2> ResolveTextLayerVisibleLocalCenter(const TextLayerRuntimeState& state) {
    if (state.primitive == nullptr || state.object.opaquebackground) return { 0.0f, 0.0f };
    return ResolveTextCropLocalCenter(state.object,
                                      state.object.origin,
                                      state.primitive->layout.glyph_offset);
}

Eigen::Vector3f ResolveTextPlacementLocalOffset(std::string_view     alignment,
                                                std::array<float, 2> placement_size) {
    // Text placement is anchored to the authored logical text box. Cropping can shrink the glyph
    // texture and move the visible quad inside that box, but it must not change the meaning of
    // `origin`, `anchor`, or top/left/right/bottom alignment.
    return AlignmentOffset(alignment, placement_size);
}

Eigen::Vector3f ResolveTextLayerPlacementLocalOffset(const TextLayerRuntimeState& state,
                                                     std::string_view             alignment) {
    return ResolveTextPlacementLocalOffset(alignment, ResolveTextPlacementDisplaySize(state));
}

Eigen::Affine3f BuildTextPlacementLocalTransform(const wpscene::WPTextObject& object,
                                                 const Eigen::Vector3f&       origin,
                                                 const Eigen::Vector3f&       local_offset) {
    // Mirror SceneNode::GetLocalTrans() exactly for candidate screen-anchor bounds. Using the same
    // transform order here keeps collision/snapping math aligned with the actual draw transform while
    // avoiding temporary SceneNode mutation during the placement solve.
    Eigen::Affine3f transform = Eigen::Affine3f::Identity();
    transform.prescale(Eigen::Vector3f { object.scale[0], object.scale[1], object.scale[2] });
    transform.prerotate(Eigen::AngleAxisf(object.angles[0], Eigen::Vector3f::UnitX()));
    transform.prerotate(Eigen::AngleAxisf(object.angles[1], Eigen::Vector3f::UnitY()));
    transform.prerotate(Eigen::AngleAxisf(object.angles[2], Eigen::Vector3f::UnitZ()));
    transform.pretranslate(origin);
    transform.translate(local_offset);
    return transform;
}

bool NearlyEqual(const Eigen::Vector3f& lhs, const Eigen::Vector3f& rhs) {
    return (lhs - rhs).squaredNorm() <= 0.000001f;
}

bool TextMetricChanged(std::array<float, 2> lhs, std::array<float, 2> rhs) {
    return std::abs(lhs[0] - rhs[0]) > 0.001f || std::abs(lhs[1] - rhs[1]) > 0.001f;
}

void ApplyTextLayerNodeAlignmentOffset(SceneNode* node, const TextLayerRuntimeState& state) {
    if (node == nullptr) return;
    node->SetAlignmentOffset(
        ResolveTextLayerPlacementLocalOffset(state, ResolveTextLayerSceneAlignment(state.object)));
}

void RebuildTextMesh(SceneMesh* mesh, std::array<float, 2> size,
                     std::array<float, 2> local_center = { 0.0f, 0.0f }) {
    if (mesh == nullptr) return;

    SceneMesh   rebuilt(mesh->Dynamic());
    const float left   = local_center[0] - (size[0] / 2.0f);
    const float right  = local_center[0] + (size[0] / 2.0f);
    const float bottom = local_center[1] - (size[1] / 2.0f);
    const float top    = local_center[1] + (size[1] / 2.0f);

    const std::array pos = {
        left, bottom, 0.0f, left, top, 0.0f, right, bottom, 0.0f, right, top, 0.0f,
    };
    const std::array texcoord { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f };

    SceneVertexArray vertex(
        std::vector<SceneVertexArray::SceneVertexAttribute> {
            { std::string(WE_IN_POSITION), VertexType::FLOAT3 },
            { std::string(WE_IN_TEXCOORD), VertexType::FLOAT2 },
        },
        4);
    vertex.SetVertex(WE_IN_POSITION, pos);
    vertex.SetVertex(WE_IN_TEXCOORD, texcoord);
    rebuilt.AddVertexArray(std::move(vertex));
    mesh->ChangeMeshDataFrom(rebuilt);
    mesh->SetDirty();
}

std::array<float, 2> ResolveTextVisibleLocalCenter(const SceneTextPrimitive& primitive) {
    if (primitive.object.opaquebackground) return { 0.0f, 0.0f };
    return ResolveTextCropLocalCenter(primitive.object,
                                      primitive.object.origin,
                                      primitive.layout.glyph_offset);
}

std::array<float, 2> ResolveTextGlyphPageLocalOffset(const SceneTextPrimitive& primitive) {
    if (primitive.object.opaquebackground) return primitive.layout.glyph_offset;
    if (!primitive.object.effects.empty()) return { 0.0f, 0.0f };
    return ResolveTextVisibleLocalCenter(primitive);
}

bool IsCameraLinkedFromScene(const Scene& scene, std::string_view camera_name) {
    return std::any_of(
        scene.linkedCameras.begin(), scene.linkedCameras.end(), [camera_name](const auto& entry) {
            const auto& linked = entry.second;
            return std::find(linked.begin(), linked.end(), camera_name) != linked.end();
        });
}

bool HasExplicitTextScreenAnchor(const wpscene::WPTextObject& object) {
    return ! object.anchor.empty() && object.anchor != "none";
}

bool TextAnchorContains(std::string_view anchor, std::string_view token) {
    return anchor.find(token) != std::string_view::npos;
}

bool HasDirectionalTextScreenAnchor(const wpscene::WPTextObject& object) {
    if (! HasExplicitTextScreenAnchor(object)) return false;
    return TextAnchorContains(object.anchor, "left") || TextAnchorContains(object.anchor, "right") ||
           TextAnchorContains(object.anchor, "top") || TextAnchorContains(object.anchor, "bottom");
}

std::string ResolveTextContentAlignment(const wpscene::WPTextObject& object) {
    std::string alignment;
    if (object.verticalalign == "top") alignment += "top";
    if (object.verticalalign == "bottom") alignment += "bottom";
    if (object.horizontalalign == "left") alignment += "left";
    if (object.horizontalalign == "right") alignment += "right";
    return alignment;
}

struct ScreenAnchorFrame {
    double view_left { 0.0 };
    double view_right { 0.0 };
    double view_bottom { 0.0 };
    double view_top { 0.0 };
    double scene_left { 0.0 };
    double scene_right { 0.0 };
    double scene_bottom { 0.0 };
    double scene_top { 0.0 };
    double scene_center_x { 0.0 };
    double scene_center_y { 0.0 };
    double view_center_x { 0.0 };
    double view_center_y { 0.0 };
};

std::optional<ScreenAnchorFrame> ResolveScreenAnchorFrame(const Scene& scene) {
    const auto* camera = scene.activeCamera;
    if (camera == nullptr || camera->IsPerspective() || scene.ortho[0] <= 0 || scene.ortho[1] <= 0) {
        return std::nullopt;
    }

    const auto camera_position = camera->GetPosition();
    ScreenAnchorFrame frame;
    frame.view_left = camera_position.x() - camera->Width() * 0.5;
    frame.view_right = camera_position.x() + camera->Width() * 0.5;
    frame.view_bottom = camera_position.y() - camera->Height() * 0.5;
    frame.view_top = camera_position.y() + camera->Height() * 0.5;
    frame.scene_left = 0.0;
    frame.scene_right = static_cast<double>(scene.ortho[0]);
    frame.scene_bottom = 0.0;
    frame.scene_top = static_cast<double>(scene.ortho[1]);
    frame.scene_center_x = frame.scene_right * 0.5;
    frame.scene_center_y = frame.scene_top * 0.5;
    frame.view_center_x = (frame.view_left + frame.view_right) * 0.5;
    frame.view_center_y = (frame.view_bottom + frame.view_top) * 0.5;
    return frame;
}

std::array<float, 3> ResolveScreenAnchoredTextOrigin(const ScreenAnchorFrame& frame,
                                                     const wpscene::WPTextObject& object) {
    std::array<float, 3> origin = object.origin;
    const std::string_view anchor = object.anchor;
    // Wallpaper Engine's text `anchor` is a screen-anchor contract, not just a local glyph-box
    // alignment hint. When fill mode narrows or widens the active orthographic camera, authored
    // edge coordinates such as dino_run's x=341.43 must follow the visible camera edge; otherwise
    // right/top anchored UI text can be clipped even though the scene.json coordinates are valid.
    if (TextAnchorContains(anchor, "left")) {
        origin[0] += static_cast<float>(frame.view_left - frame.scene_left);
    } else if (TextAnchorContains(anchor, "right")) {
        origin[0] += static_cast<float>(frame.view_right - frame.scene_right);
    } else {
        origin[0] += static_cast<float>(frame.view_center_x - frame.scene_center_x);
    }

    if (TextAnchorContains(anchor, "bottom")) {
        origin[1] += static_cast<float>(frame.view_bottom - frame.scene_bottom);
    } else if (TextAnchorContains(anchor, "top")) {
        origin[1] += static_cast<float>(frame.view_top - frame.scene_top);
    } else {
        origin[1] += static_cast<float>(frame.view_center_y - frame.scene_center_y);
    }

    return origin;
}

struct ScreenAnchoredTextBounds {
    float left { 0.0f };
    float right { 0.0f };
    float bottom { 0.0f };
    float top { 0.0f };
};

struct ScreenAnchoredTextPlacement {
    int32_t                layer_id { 0 };
    TextLayerRuntimeState* state { nullptr };
    SceneNode*             node { nullptr };
    Eigen::Vector3f        translation { 0.0f, 0.0f, 0.0f };
    ScreenAnchoredTextBounds bounds;
};

ScreenAnchoredTextBounds ResolveScreenAnchoredTextBounds(const TextLayerRuntimeState& state,
                                                         const Eigen::Vector3f& translation) {
    const auto visible_display_size = ResolveVisibleTextDisplaySize(state);
    const auto visible_local_center = ResolveTextLayerVisibleLocalCenter(state);
    const auto alignment = ResolveTextLayerSceneAlignment(state.object);
    const auto local_offset = ResolveTextLayerPlacementLocalOffset(state, alignment);
    const auto transform = BuildTextPlacementLocalTransform(state.object, translation, local_offset);
    const float half_width = visible_display_size[0] * 0.5f;
    const float half_height = visible_display_size[1] * 0.5f;
    const std::array<Eigen::Vector3f, 4> corners {
        Eigen::Vector3f { visible_local_center[0] - half_width,
                          visible_local_center[1] - half_height,
                          0.0f },
        Eigen::Vector3f { visible_local_center[0] - half_width,
                          visible_local_center[1] + half_height,
                          0.0f },
        Eigen::Vector3f { visible_local_center[0] + half_width,
                          visible_local_center[1] - half_height,
                          0.0f },
        Eigen::Vector3f { visible_local_center[0] + half_width,
                          visible_local_center[1] + half_height,
                          0.0f },
    };

    ScreenAnchoredTextBounds bounds {
        .left = std::numeric_limits<float>::max(),
        .right = std::numeric_limits<float>::lowest(),
        .bottom = std::numeric_limits<float>::max(),
        .top = std::numeric_limits<float>::lowest(),
    };
    for (const auto& corner : corners) {
        const Eigen::Vector3f world = transform * corner;
        bounds.left = std::min(bounds.left, world.x());
        bounds.right = std::max(bounds.right, world.x());
        bounds.bottom = std::min(bounds.bottom, world.y());
        bounds.top = std::max(bounds.top, world.y());
    }
    return bounds;
}

bool ScreenAnchoredTextOverlapsHorizontally(const ScreenAnchoredTextBounds& lhs,
                                            const ScreenAnchoredTextBounds& rhs) {
    return lhs.left < rhs.right && rhs.left < lhs.right;
}

void MoveScreenAnchoredTextPlacementY(ScreenAnchoredTextPlacement& placement, float delta_y) {
    placement.translation.y() += delta_y;
    placement.bounds.bottom += delta_y;
    placement.bounds.top += delta_y;
}

void MoveScreenAnchoredTextPlacementX(ScreenAnchoredTextPlacement& placement, float delta_x) {
    placement.translation.x() += delta_x;
    placement.bounds.left += delta_x;
    placement.bounds.right += delta_x;
}

void SnapScreenAnchoredTextPlacementToFrame(ScreenAnchoredTextPlacement& placement,
                                            const ScreenAnchorFrame&       frame) {
    const auto* state = placement.state;
    if (state == nullptr) return;

    const std::string_view anchor = state->object.anchor;
    if (TextAnchorContains(anchor, "left")) {
        const float authored_left_inset = std::max(
            0.0f, state->object.origin[0] - static_cast<float>(frame.scene_left));
        MoveScreenAnchoredTextPlacementX(
            placement,
            static_cast<float>(frame.view_left) + authored_left_inset - placement.bounds.left);
    } else if (TextAnchorContains(anchor, "right")) {
        const float authored_right_inset = std::max(
            0.0f, static_cast<float>(frame.scene_right) - state->object.origin[0]);
        MoveScreenAnchoredTextPlacementX(
            placement,
            static_cast<float>(frame.view_right) - authored_right_inset - placement.bounds.right);
    }

    if (TextAnchorContains(anchor, "bottom")) {
        const float authored_bottom_inset = std::max(
            0.0f, state->object.origin[1] - static_cast<float>(frame.scene_bottom));
        MoveScreenAnchoredTextPlacementY(
            placement,
            static_cast<float>(frame.view_bottom) + authored_bottom_inset - placement.bounds.bottom);
    } else if (TextAnchorContains(anchor, "top")) {
        // The screen anchor must be resolved against the actual visible glyph rectangle after
        // Pango/Freetype cropping, not merely against the authored `origin`. Preserve the authored
        // inset from the project canvas edge so Windows-style HUD labels keep their small breathing
        // room while still removing Linux font-metric slack from the visible edge calculation.
        const float authored_top_inset = std::max(
            0.0f, static_cast<float>(frame.scene_top) - state->object.origin[1]);
        MoveScreenAnchoredTextPlacementY(
            placement,
            static_cast<float>(frame.view_top) - authored_top_inset - placement.bounds.top);
    }
}

bool ResolveTopScreenAnchoredTextStack(std::vector<ScreenAnchoredTextPlacement>& placements) {
    std::vector<size_t> top_indices;
    for (size_t index = 0; index < placements.size(); ++index) {
        const auto* state = placements[index].state;
        if (state != nullptr && TextAnchorContains(state->object.anchor, "top")) {
            top_indices.push_back(index);
        }
    }

    std::sort(top_indices.begin(), top_indices.end(), [&](size_t lhs, size_t rhs) {
        const auto& left = placements[lhs];
        const auto& right = placements[rhs];
        const float left_authored_y = left.state != nullptr ? left.state->object.origin[1] : left.bounds.top;
        const float right_authored_y =
            right.state != nullptr ? right.state->object.origin[1] : right.bounds.top;
        if (std::abs(left_authored_y - right_authored_y) > 0.0001f) {
            return left_authored_y > right_authored_y;
        }
        return left.layer_id < right.layer_id;
    });

    bool changed_any = false;
    for (size_t ordered_index = 1; ordered_index < top_indices.size(); ++ordered_index) {
        auto& current = placements[top_indices[ordered_index]];
        float required_down_shift = 0.0f;
        for (size_t previous_order = 0; previous_order < ordered_index; ++previous_order) {
            const auto& previous = placements[top_indices[previous_order]];
            if (! ScreenAnchoredTextOverlapsHorizontally(current.bounds, previous.bounds)) continue;

            // Multiple top-anchored Wallpaper Engine labels can deliberately share the same screen
            // corner, as dino_run does with `label_coins` and `label_top`. Linux/Pango text bounds
            // can be taller than the authored Windows raster, so use the actual visible rectangles
            // and keep the lower label below the previous one instead of trusting the raw origin
            // delta to be enough for every font backend.
            const float target_top = previous.bounds.bottom - kScreenAnchoredTextStackGap;
            if (current.bounds.top > target_top) {
                required_down_shift = std::max(required_down_shift, current.bounds.top - target_top);
            }
        }
        if (required_down_shift > 0.0001f) {
            MoveScreenAnchoredTextPlacementY(current, -required_down_shift);
            changed_any = true;
        }
    }
    return changed_any;
}

bool ResolveBottomScreenAnchoredTextStack(std::vector<ScreenAnchoredTextPlacement>& placements) {
    std::vector<size_t> bottom_indices;
    for (size_t index = 0; index < placements.size(); ++index) {
        const auto* state = placements[index].state;
        if (state != nullptr && TextAnchorContains(state->object.anchor, "bottom")) {
            bottom_indices.push_back(index);
        }
    }

    std::sort(bottom_indices.begin(), bottom_indices.end(), [&](size_t lhs, size_t rhs) {
        const auto& left = placements[lhs];
        const auto& right = placements[rhs];
        const float left_authored_y =
            left.state != nullptr ? left.state->object.origin[1] : left.bounds.bottom;
        const float right_authored_y =
            right.state != nullptr ? right.state->object.origin[1] : right.bounds.bottom;
        if (std::abs(left_authored_y - right_authored_y) > 0.0001f) {
            return left_authored_y < right_authored_y;
        }
        return left.layer_id < right.layer_id;
    });

    bool changed_any = false;
    for (size_t ordered_index = 1; ordered_index < bottom_indices.size(); ++ordered_index) {
        auto& current = placements[bottom_indices[ordered_index]];
        float required_up_shift = 0.0f;
        for (size_t previous_order = 0; previous_order < ordered_index; ++previous_order) {
            const auto& previous = placements[bottom_indices[previous_order]];
            if (! ScreenAnchoredTextOverlapsHorizontally(current.bounds, previous.bounds)) continue;

            // Bottom-anchored labels need the mirror image of the top stack: preserve the authored
            // ordering from the edge, but raise later labels only when their actual visible glyph
            // rectangles collide with earlier labels on the same horizontal run.
            const float target_bottom = previous.bounds.top + kScreenAnchoredTextStackGap;
            if (current.bounds.bottom < target_bottom) {
                required_up_shift = std::max(required_up_shift, target_bottom - current.bounds.bottom);
            }
        }
        if (required_up_shift > 0.0001f) {
            MoveScreenAnchoredTextPlacementY(current, required_up_shift);
            changed_any = true;
        }
    }
    return changed_any;
}

void SyncTextLayerEffectTransform(Scene& scene, int32_t layer_id, SceneNode* node) {
    if (node == nullptr) return;

    auto camera_names_it = scene.objectRuntimeCameraNames.find(layer_id);
    if (camera_names_it == scene.objectRuntimeCameraNames.end()) return;

    for (const auto& camera_name : camera_names_it->second) {
        auto camera_it = scene.cameras.find(camera_name);
        if (camera_it == scene.cameras.end() || ! camera_it->second->HasImgEffect()) continue;

        auto* effect_layer = camera_it->second->GetImgEffect().get();
        if (effect_layer == nullptr) continue;

        // Screen-anchor adjustments mutate the visible world node, so effect-backed text needs the
        // same final-composite resynchronization that script-driven origin/scale writes already
        // perform. The offscreen source camera stays in local text space; only the resolved output
        // node follows the anchored world transform.
        if (effect_layer->WorldNode() != nullptr) {
            effect_layer->SyncResolvedNodeToWorld();
        } else {
            effect_layer->SyncResolvedNodeToMatrix(
                Eigen::Affine3f(node->GetLocalTrans().cast<float>()));
        }
    }
}

} // namespace

void wallpaper::RebuildTextPrimitiveVisibleMesh(SceneMesh* mesh,
                                                const SceneTextPrimitive& primitive) {
    if (mesh == nullptr) return;
    RebuildTextMesh(mesh, primitive.VisibleDisplaySize(), ResolveTextVisibleLocalCenter(primitive));
}

std::string wallpaper::ResolveTextLayerSceneAlignment(const wpscene::WPTextObject& object) {
    // Directional anchors bind the layer to a screen edge and also provide the local placement edge.
    // A plain center anchor only moves the layer with the screen center; text layout still follows
    // horizontalalign/verticalalign inside the authored text box.
    if (HasDirectionalTextScreenAnchor(object)) return object.anchor;
    return ResolveTextContentAlignment(object);
}

TextLayerPropertyUpdateStrategy wallpaper::ResolveTextLayerPropertyUpdateStrategy(
    const TextLayerRuntimeState& state,
    std::string_view             property_name) {
    (void)state;
    if (property_name == "alpha" || property_name == "color" ||
        property_name == "backgroundcolor" || property_name == "backgroundbrightness") {
        return TextLayerPropertyUpdateStrategy::MaterialOnly;
    }
    if (property_name == "anchor") return TextLayerPropertyUpdateStrategy::TransformOnly;
    if (property_name == "opaquebackground") {
        return TextLayerPropertyUpdateStrategy::BridgeResourceResize;
    }
    return TextLayerPropertyUpdateStrategy::LayoutOnly;
}

bool wpscene::WPTextObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    (void)vfs;
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "origin", origin);
    GET_JSON_NAME_VALUE_NOWARN(json, "scale", scale);
    GET_JSON_NAME_VALUE_NOWARN(json, "angles", angles);
    GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
    GET_JSON_NAME_VALUE_NOWARN(json, "size", size);
    ReadLiteralOrDynamicValue(json, "text", &text);
    ReadLiteralOrDynamicValue(json, "font", &font);
    ReadLiteralOrDynamicValue(json, "color", &color);
    ReadLiteralOrDynamicValue(json, "backgroundcolor", &backgroundcolor);
    ReadLiteralOrDynamicValue(json, "backgroundbrightness", &backgroundbrightness);
    ReadLiteralOrDynamicValue(json, "alpha", &alpha);
    ReadLiteralOrDynamicValue(json, "pointsize", &pointsize);
    ReadLiteralOrDynamicValue(json, "maxwidth", &maxwidth);
    ReadLiteralOrDynamicValue(json, "maxrows", &maxrows);
    ReadTextPaddingValue(json, id, &padding, &padding_edges);
    GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);
    GET_JSON_NAME_VALUE_NOWARN(json, "attachment", attachment);
    ReadLiteralOrDynamicValue(json, "visible", &visible);
    ReadLiteralOrDynamicValue(json, "opaquebackground", &opaquebackground);
    ReadLiteralOrDynamicValue(json, "blockalign", &blockalign);
    ReadLiteralOrDynamicValue(json, "limitrows", &limitrows);
    ReadLiteralOrDynamicValue(json, "limituseellipsis", &limituseellipsis);
    ReadLiteralOrDynamicValue(json, "limitwidth", &limitwidth);
    ReadLiteralOrDynamicValue(json, "horizontalalign", &horizontalalign);
    ReadLiteralOrDynamicValue(json, "verticalalign", &verticalalign);
    ReadLiteralOrDynamicValue(json, "anchor", &anchor);
    ReadLiteralOrDynamicValue(json, "depthtest", &depthtest);

    size_explicit = json.contains("size") && ! json.at("size").is_null();
    if (json.contains("visible")) {
        ReadVisibleBinding(json.at("visible"), &visible_binding);
        has_visible_script = json.at("visible").is_object() &&
                             json.at("visible").contains("script") &&
                             ! json.at("visible").at("script").is_null();
    }
    has_dynamic_layout_script =
        PropertyHasScriptOrAnimation(json, "text") || PropertyHasScriptOrAnimation(json, "font") ||
        PropertyHasScriptOrAnimation(json, "pointsize") ||
        PropertyHasScriptOrAnimation(json, "padding") ||
        PropertyHasScriptOrAnimation(json, "maxwidth") ||
        PropertyHasScriptOrAnimation(json, "maxrows") ||
        PropertyHasScriptOrAnimation(json, "limitwidth") ||
        PropertyHasScriptOrAnimation(json, "limitrows") ||
        PropertyHasScriptOrAnimation(json, "horizontalalign") ||
        PropertyHasScriptOrAnimation(json, "verticalalign") ||
        PropertyHasScriptOrAnimation(json, "anchor") || PropertyHasScriptOrAnimation(json, "size");

    if (json.contains("effects") && json.at("effects").is_array()) {
        for (const auto& effect_json : json.at("effects")) {
            WPImageEffect effect;
            if (effect.FromJson(effect_json, vfs)) effects.push_back(std::move(effect));
        }
    }
    return true;
}

bool wallpaper::HasTextLayerProperty(std::string_view property_name) {
    return property_name == "name" || property_name == "size" || property_name == "text" ||
           property_name == "font" || property_name == "color" || property_name == "alpha" ||
           property_name == "backgroundcolor" || property_name == "backgroundbrightness" ||
           property_name == "opaquebackground" ||
           property_name == "pointsize" || property_name == "padding" ||
           property_name == "horizontalalign" || property_name == "verticalalign" ||
           property_name == "anchor" || property_name == "limitrows" ||
           property_name == "maxrows" || property_name == "limitwidth" ||
           property_name == "maxwidth";
}

std::optional<WPDynamicValue> wallpaper::ReadTextLayerProperty(const TextLayerRuntimeState& state,
                                                               std::string_view property_name) {
    const auto& object = state.object;
    std::optional<WPDynamicValue> result;
    if (property_name == "name") {
        result = WPDynamicValue(object.name);
    } else if (property_name == "size") {
        result = WPDynamicValue(object.size);
    } else if (property_name == "text") {
        result = WPDynamicValue(object.text);
    } else if (property_name == "font") {
        result = WPDynamicValue(object.font);
    } else if (property_name == "color") {
        result = WPDynamicValue(object.color);
    } else if (property_name == "alpha") {
        result = WPDynamicValue(object.alpha);
    } else if (property_name == "backgroundcolor") {
        result = WPDynamicValue(object.backgroundcolor);
    } else if (property_name == "backgroundbrightness") {
        result = WPDynamicValue(object.backgroundbrightness);
    } else if (property_name == "opaquebackground") {
        result = WPDynamicValue(object.opaquebackground);
    } else if (property_name == "pointsize") {
        result = WPDynamicValue(object.pointsize);
    } else if (property_name == "padding") {
        result = WPDynamicValue(static_cast<int32_t>(object.padding));
    } else if (property_name == "horizontalalign") {
        result = WPDynamicValue(object.horizontalalign);
    } else if (property_name == "verticalalign") {
        result = WPDynamicValue(object.verticalalign);
    } else if (property_name == "anchor") {
        result = WPDynamicValue(object.anchor);
    } else if (property_name == "limitrows") {
        result = WPDynamicValue(object.limitrows);
    } else if (property_name == "maxrows") {
        result = WPDynamicValue(static_cast<int32_t>(object.maxrows));
    } else if (property_name == "limitwidth") {
        result = WPDynamicValue(object.limitwidth);
    } else if (property_name == "maxwidth") {
        result = WPDynamicValue(object.maxwidth);
    }

    // Property reads sit on the per-frame script hot path. Logging every successful read made
    // unchanged Clock/Date/Day scripts emit thousands of lines per minute and could dominate the
    // minute-rollover cost we are trying to measure. Keep diagnostics on mutations/layout/resource
    // events instead, where they describe real renderer work rather than ordinary value polling.
    return result;
}

bool wallpaper::ApplyTextLayerDisplaySize(TextLayerRuntimeState& state,
                                          std::array<float, 2> display_size, double render_scale) {
    display_size[0] = std::max(display_size[0], 1.0f);
    display_size[1] = std::max(display_size[1], 1.0f);

    state.object.size = display_size;
    // Size writes now only mutate authored state. The live primitive keeps the previously applied
    // geometry until the caller chooses one of the explicit runtime actions. The rasterizer owns
    // the display-space to Pango-layout conversion, so runtime scripts can round-trip
    // `thisLayer.size` without receiving or storing hidden HiDPI layout units.
    (void)render_scale;
    state.object.size_explicit = true;
    return true;
}

bool wallpaper::ApplyTextLayerPropertyValue(TextLayerRuntimeState& state,
                                            std::string_view       property_name,
                                            const WPDynamicValue&  value) {
    auto& object = state.object;
    bool       applied        = false;

    if (property_name == "name") {
        applied = value.tryGet(&object.name);
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
    } else if (property_name == "horizontalalign") {
        applied = value.tryGet(&object.horizontalalign);
    } else if (property_name == "verticalalign") {
        applied = value.tryGet(&object.verticalalign);
    } else if (property_name == "anchor") {
        applied = value.tryGet(&object.anchor);
    } else if (property_name == "limitrows") {
        applied = value.tryGet(&object.limitrows);
    } else if (property_name == "limitwidth") {
        applied = value.tryGet(&object.limitwidth);
    } else if (property_name == "maxwidth") {
        applied = value.tryGet(&object.maxwidth);
    }

    if (property_name == "padding") {
        int32_t padding = 0;
        if (! value.tryGet(&padding)) {
            LOG_ERROR("TextLayerPropertyApply: layer=%d name='%s' property='padding' "
                      "failed to read scalar runtime value",
                      object.id,
                      object.name.c_str());
            return false;
        }
        object.padding       = padding;
        object.padding_edges = UniformTextPadding(padding);
        applied = true;
    }
    if (!applied && property_name == "maxrows") {
        int32_t maxrows = 0;
        if (! value.tryGet(&maxrows)) return false;
        object.maxrows = maxrows;
        applied = true;
    }

    return applied;
}

namespace
{

std::string MakeTextLayerTextureKey(int32_t object_id) {
    return "__hanabi_text_layer_" + std::to_string(object_id);
}

std::string MakeTextLayerGlyphPageTextureKey(int32_t object_id, uint32_t page_index) {
    return MakeTextLayerTextureKey(object_id) + "__glyph_page_" + std::to_string(page_index);
}

std::vector<SceneNode*> FindTextPrimitiveRuntimeNodes(Scene& scene, int32_t layer_id) {
    std::vector<SceneNode*> nodes;
    if (const auto runtime_nodes_it = scene.objectRuntimeNodes.find(layer_id);
        runtime_nodes_it != scene.objectRuntimeNodes.end()) {
        for (auto* node : runtime_nodes_it->second) {
            if (node != nullptr && node->Text() != nullptr) nodes.push_back(node);
        }
    }
    if (nodes.empty()) {
        if (auto layer_node_it = scene.layerNodes.find(layer_id);
            layer_node_it != scene.layerNodes.end() && layer_node_it->second != nullptr &&
            layer_node_it->second->Text() != nullptr) {
            nodes.push_back(layer_node_it->second);
        }
    }
    return nodes;
}

TextLayoutResult BuildCanonicalTextLayoutResult(const wpscene::WPTextObject& object,
                                                const TextRasterLayoutResult& generated) {
    TextLayoutResult result;
    result.logical_size = generated.logical_size;
    result.logical_source_size = generated.logical_source_size;
    result.glyph_display_size = generated.glyph_display_size;
    result.glyph_source_size = generated.glyph_source_size;
    result.glyph_offset = generated.glyph_offset;
    result.visible_display_size = object.opaquebackground ? generated.logical_size
                                                          : generated.glyph_display_size;
    result.visible_source_size = object.opaquebackground ? generated.logical_source_size
                                                         : generated.glyph_source_size;
    // The visible offset records where cropped glyph bounds sit relative to the authored logical
    // box. Node placement consumes the logical box instead; this offset remains available for
    // background composition, diagnostics, and compatibility with callers that need the crop delta.
    result.visible_display_offset =
        object.opaquebackground ? std::array<float, 2> { 0.0f, 0.0f } : generated.display_offset;
    result.glyph_pages.reserve(generated.glyph_pages.size());
    for (size_t page_index = 0; page_index < generated.glyph_pages.size(); page_index++) {
        result.glyph_pages.push_back(TextGlyphAtlasPage {
            .texture_key = MakeTextLayerGlyphPageTextureKey(object.id, static_cast<uint32_t>(page_index)),
            .image = generated.glyph_pages[page_index].image,
            .source_size = generated.glyph_pages[page_index].source_size,
        });
    }
    result.glyph_runs.reserve(generated.glyph_quads.size());
    for (const auto& quad : generated.glyph_quads) {
        result.glyph_runs.push_back(TextGlyphRun {
            .page_index = quad.page_index,
            .source_rect = quad.source_rect,
            .atlas_rect = quad.atlas_rect,
        });
    }
    return result;
}

std::shared_ptr<SceneMesh> BuildTextPrimitiveBackgroundMesh(const SceneTextPrimitive& primitive) {
    auto mesh = std::make_shared<SceneMesh>(true);
    GenCardMesh(*mesh,
                { ResolveTextMeshExtent(primitive.layout.logical_size[0]),
                  ResolveTextMeshExtent(primitive.layout.logical_size[1]) });
    mesh->SetDirty();
    return mesh;
}

std::shared_ptr<SceneMesh> BuildTextPrimitiveGlyphPageMesh(const SceneTextPrimitive& primitive,
                                                           uint32_t                  page_index) {
    auto mesh = std::make_shared<SceneMesh>(true);

    const auto page_count = primitive.layout.glyph_pages.size();
    if (page_index >= page_count || primitive.layout.glyph_source_size[0] <= 0.0f ||
        primitive.layout.glyph_source_size[1] <= 0.0f) {
        mesh->SetDirty();
        return mesh;
    }

    const auto page_source_size = primitive.layout.glyph_pages[page_index].source_size;
    if (page_source_size[0] <= 0.0f || page_source_size[1] <= 0.0f) {
        mesh->SetDirty();
        return mesh;
    }

    const float scale_x = primitive.layout.glyph_display_size[0] / primitive.layout.glyph_source_size[0];
    const float scale_y = primitive.layout.glyph_display_size[1] / primitive.layout.glyph_source_size[1];
    const auto glyph_local_offset = ResolveTextGlyphPageLocalOffset(primitive);

    std::vector<TextGlyphRun> page_runs;
    page_runs.reserve(primitive.layout.glyph_runs.size());
    for (const auto& run : primitive.layout.glyph_runs) {
        if (run.page_index == page_index) page_runs.push_back(run);
    }

    if (page_runs.empty()) {
        mesh->SetDirty();
        return mesh;
    }

    std::vector<float> positions;
    std::vector<float> texcoords;
    std::vector<uint16_t> indices;
    positions.reserve(page_runs.size() * 12);
    texcoords.reserve(page_runs.size() * 8);
    indices.reserve(page_runs.size() * 6);

    uint16_t vertex_base = 0;
    for (const auto& run : page_runs) {
        const float rect_x = run.source_rect[0] * scale_x;
        const float rect_y = run.source_rect[1] * scale_y;
        const float rect_width = run.source_rect[2] * scale_x;
        const float rect_height = run.source_rect[3] * scale_y;

        const float left = -primitive.layout.glyph_display_size[0] * 0.5f + rect_x + glyph_local_offset[0];
        const float right = left + rect_width;
        const float top = primitive.layout.glyph_display_size[1] * 0.5f - rect_y + glyph_local_offset[1];
        const float bottom = top - rect_height;

        positions.insert(positions.end(),
                         { left, bottom, 0.0f, left, top, 0.0f, right, bottom, 0.0f, right, top, 0.0f });

        const float u0 = run.atlas_rect[0] / page_source_size[0];
        const float v0 = run.atlas_rect[1] / page_source_size[1];
        const float u1 = (run.atlas_rect[0] + run.atlas_rect[2]) / page_source_size[0];
        const float v1 = (run.atlas_rect[1] + run.atlas_rect[3]) / page_source_size[1];
        texcoords.insert(texcoords.end(), { u0, v1, u0, v0, u1, v1, u1, v0 });

        // The direct text pass still renders glyph pages as indexed quads; using the standard
        // rectangle split keeps the dedicated text pipeline numerically identical to the
        // previously verified atlas geometry while removing the old helper-child scene nodes.
        indices.insert(indices.end(),
                       { vertex_base,
                         static_cast<uint16_t>(vertex_base + 1),
                         static_cast<uint16_t>(vertex_base + 2),
                         static_cast<uint16_t>(vertex_base + 1),
                         static_cast<uint16_t>(vertex_base + 3),
                         static_cast<uint16_t>(vertex_base + 2) });
        vertex_base = static_cast<uint16_t>(vertex_base + 4);
    }

    const std::vector<SceneVertexArray::SceneVertexAttribute> vertex_attributes {
        { std::string(WE_IN_POSITION), VertexType::FLOAT3 },
        { std::string(WE_IN_TEXCOORD), VertexType::FLOAT2 },
    };
    SceneVertexArray vertex(vertex_attributes, page_runs.size() * 4);
    vertex.SetVertex(WE_IN_POSITION, positions);
    vertex.SetVertex(WE_IN_TEXCOORD, texcoords);
    mesh->AddVertexArray(std::move(vertex));

    SceneIndexArray index_array(page_runs.size() * 2);
    index_array.AssignHalf(0, indices);
    mesh->AddIndexArray(std::move(index_array));
    mesh->SetDirty();
    return mesh;
}

struct TextLayerSceneGeometrySnapshot {
    std::string         alignment;
    std::array<float, 2> placement_display_size { 0.0f, 0.0f };
    std::array<float, 2> visible_display_size { 0.0f, 0.0f };
    std::array<float, 2> visible_source_size { 0.0f, 0.0f };
    std::array<float, 2> dependency_source_size { 0.0f, 0.0f };
    std::array<float, 2> visible_local_center { 0.0f, 0.0f };
};

TextLayerSceneGeometrySnapshot CaptureTextLayerSceneGeometry(const TextLayerRuntimeState& state) {
    const auto alignment =
        state.applied_alignment.empty() ? ResolveTextLayerSceneAlignment(state.object)
                                        : state.applied_alignment;
    return TextLayerSceneGeometrySnapshot {
        .alignment = alignment,
        .placement_display_size = ResolveTextPlacementDisplaySize(state),
        .visible_display_size = ResolveVisibleTextDisplaySize(state),
        .visible_source_size = ResolveVisibleTextSourceSize(state),
        .dependency_source_size = ResolveEffectDependencyTextSourceSize(state),
        .visible_local_center = ResolveTextLayerVisibleLocalCenter(state),
    };
}

void SyncTextPrimitiveCanonicalState(TextLayerRuntimeState& state, bool rebuild_runtime_meshes) {
    if (state.primitive == nullptr) return;

    // The runtime keeps one authoritative text primitive alive across property updates. Cheap
    // changes therefore synchronize the authored object back onto that primitive and then refresh
    // only the canonical geometry fields that downstream passes consume. Layout/shaping data is
    // preserved unless the caller explicitly requests a full layout rebuild.
    state.applied_alignment = ResolveTextLayerSceneAlignment(state.object);
    state.primitive->object = state.object;
    state.primitive->layout.visible_display_size = ResolveVisibleTextDisplaySize(state);
    state.primitive->layout.visible_source_size = ResolveVisibleTextSourceSize(state);
    state.primitive->layout.visible_display_offset =
        ResolveVisibleTextDisplayOffset(state, state.applied_alignment);
    state.primitive->bridge.source_size = ResolveEffectDependencyTextSourceSize(state);

    if (!rebuild_runtime_meshes) return;

    // Opaque-background toggles and similar geometry-only updates do not require reshaping glyphs,
    // but they do change how the already-shaped glyph pages are placed inside the canonical text
    // box. Rebuilding just the meshes keeps the direct text pass and the bridge source aligned
    // with the new visibility contract without rerasterizing the atlas itself.
    if (state.primitive->background_mesh != nullptr) {
        state.primitive->background_mesh = BuildTextPrimitiveBackgroundMesh(*state.primitive);
    }
    state.primitive->glyph_pages.resize(state.primitive->layout.glyph_pages.size());
    for (size_t page_index = 0; page_index < state.primitive->layout.glyph_pages.size();
         page_index++) {
        const auto& layout_page = state.primitive->layout.glyph_pages[page_index];
        auto&       renderable = state.primitive->glyph_pages[page_index];
        renderable.page_index = static_cast<uint32_t>(page_index);
        renderable.texture_key = layout_page.texture_key;
        renderable.source_size = layout_page.source_size;
        renderable.mesh = BuildTextPrimitiveGlyphPageMesh(*state.primitive,
                                                          static_cast<uint32_t>(page_index));
    }
}

bool ApplyTextLayerSceneGeometry(Scene&                         scene,
                                 int32_t                        layer_id,
                                 TextLayerRuntimeState&         state,
                                 const TextLayerSceneGeometrySnapshot& previous_geometry) {
    const auto next_geometry = CaptureTextLayerSceneGeometry(state);
    const bool placement_display_size_changed =
        TextMetricChanged(previous_geometry.placement_display_size,
                          next_geometry.placement_display_size);
    const bool visible_display_size_changed =
        TextMetricChanged(previous_geometry.visible_display_size, next_geometry.visible_display_size);
    const bool dependency_source_size_changed =
        TextMetricChanged(previous_geometry.dependency_source_size,
                          next_geometry.dependency_source_size);
    const bool visible_local_center_changed =
        TextMetricChanged(previous_geometry.visible_local_center,
                          next_geometry.visible_local_center);
    const bool alignment_changed = previous_geometry.alignment != next_geometry.alignment;

    if (placement_display_size_changed || alignment_changed) {
        if (auto layer_node_it = scene.layerNodes.find(layer_id);
            layer_node_it != scene.layerNodes.end()) {
            ApplyTextLayerNodeAlignmentOffset(layer_node_it->second, state);
        }
    }

    bool       bridge_render_target_changed = false;
    const auto  camera_names_it = scene.objectRuntimeCameraNames.find(layer_id);
    const bool  has_runtime_cameras = camera_names_it != scene.objectRuntimeCameraNames.end();
    if (has_runtime_cameras) {
        const auto camera_size = next_geometry.visible_display_size;
        for (const auto& camera_name : camera_names_it->second) {
            auto camera_it = scene.cameras.find(camera_name);
            if (camera_it == scene.cameras.end()) continue;

            if (!IsCameraLinkedFromScene(scene, camera_name)) {
                camera_it->second->SetWidth(std::max(1.0, static_cast<double>(camera_size[0])));
                camera_it->second->SetHeight(std::max(1.0, static_cast<double>(camera_size[1])));
                camera_it->second->Update();
            }

            if (!camera_it->second->HasImgEffect()) continue;

            auto& effect_layer = *camera_it->second->GetImgEffect();
            // Text bridges now expose exact-size source images. Once the canonical visible box is
            // known, every downstream effect pass can keep using a plain full-UV card; runtime text
            // updates only need to resize that card to the latest visible display size.
            RebuildTextMesh(&effect_layer.FinalMesh(),
                            camera_size,
                            next_geometry.visible_local_center);
            effect_layer.SyncResolvedOutputMesh();
            effect_layer.SyncResolvedNodeToWorld();
        }
    }

    if (state.primitive != nullptr) {
        for (const auto& bridge_target : state.primitive->bridge.render_targets) {
            auto render_target_it = scene.renderTargets.find(bridge_target.name);
            if (render_target_it == scene.renderTargets.end()) continue;
            auto& render_target = render_target_it->second;
            if (render_target.bind.enable) continue;

            const auto scaled_source_size = ResolveTextBridgeRenderTargetSourceSize(
                bridge_target, next_geometry.dependency_source_size);
            const bool dependency_rt_changed =
                GrowTextDependencyRenderTarget(render_target, scaled_source_size);
            bridge_render_target_changed = bridge_render_target_changed || dependency_rt_changed;
        }
    }

    const bool has_bridge_resources =
        state.primitive != nullptr && !state.primitive->bridge.render_targets.empty();
    if (has_bridge_resources &&
        (visible_display_size_changed || visible_local_center_changed ||
         dependency_source_size_changed ||
         bridge_render_target_changed)) {
        // The final text bridge keeps a stable pass identity and exact-size backing images. The
        // only Vulkan resources affected by a text size change are the bridge outputs and the
        // passes that sample them, so mark those render targets explicitly instead of requesting a
        // scene-wide resource refresh that would walk every unrelated effect pass on minute ticks.
        for (const auto& bridge_target : state.primitive->bridge.render_targets) {
            scene.MarkRenderTargetResourcesDirty(bridge_target.name);
        }
    }

    return true;
}

bool SyncTextLayerSceneGeometry(Scene&                         scene,
                                int32_t                        layer_id,
                                TextLayerRuntimeState&         state,
                                const TextLayerSceneGeometrySnapshot& previous_geometry) {
    SyncTextPrimitiveCanonicalState(state, false);
    const auto next_geometry = CaptureTextLayerSceneGeometry(state);
    const bool visible_local_center_changed =
        TextMetricChanged(previous_geometry.visible_local_center,
                          next_geometry.visible_local_center);

    if (visible_local_center_changed && TextLayerUsesTightTransparentGlyphBounds(state.object)) {
        // Transform-only script writes can alter the projected crop center without changing shaped
        // glyph data. The canonical primitive therefore needs a mesh-only refresh: reuse the current
        // atlas/layout, rebuild local quad positions from the new transform inputs, and mark the
        // owning TextPass so GPU buffers are refreshed before the next recorded draw.
        SyncTextPrimitiveCanonicalState(state, true);
        scene.MarkTextLayerResourcesDirty(layer_id);
    }

    return ApplyTextLayerSceneGeometry(scene, layer_id, state, previous_geometry);
}

bool ApplyTextLayerObjectTransform(TextLayerRuntimeState& state,
                                   SceneNode*            node,
                                   std::string_view      property_name,
                                   std::array<float, 3>  value) {
    if (node == nullptr) return false;

    if (property_name == "origin") {
        state.object.origin = value;
        ApplyTextLayerNodePlacement(node, state, value);
        return true;
    }

    if (property_name == "angles") {
        state.object.angles = value;
        node->SetRotation(Eigen::Vector3f { value[0], value[1], value[2] });
        ApplyTextLayerNodePlacement(node, state, state.object.origin);
        return true;
    }

    if (property_name == "scale") {
        state.object.scale = value;
        node->SetScale(Eigen::Vector3f { value[0], value[1], value[2] });
        ApplyTextLayerNodePlacement(node, state, state.object.origin);
        return true;
    }

    return false;
}

} // namespace

bool wallpaper::BuildSceneTextPrimitive(fs::VFS&                         vfs,
                                        wpscene::WPTextObject&           object,
                                        uint32_t                         texture_version,
                                        double                           render_scale,
                                        std::shared_ptr<SceneTextPrimitive>* out_primitive,
                                        std::string*                     out_error) {
    if (out_primitive == nullptr) return false;

    TextRasterLayoutResult generated;
    if (!RasterizeTextPrimitiveLayout(vfs,
                                      object,
                                      MakeTextLayerTextureKey(object.id),
                                      render_scale,
                                      &generated,
                                      out_error)) {
        return false;
    }

    auto primitive = std::make_shared<SceneTextPrimitive>();
    primitive->object = object;
    primitive->layout = BuildCanonicalTextLayoutResult(object, generated);
    primitive->atlas_version = texture_version;
    primitive->background_mesh = BuildTextPrimitiveBackgroundMesh(*primitive);
    primitive->glyph_pages.reserve(primitive->layout.glyph_pages.size());
    for (size_t page_index = 0; page_index < primitive->layout.glyph_pages.size(); page_index++) {
        auto& layout_page = primitive->layout.glyph_pages[page_index];
        if (layout_page.image != nullptr) {
            layout_page.image->revision = texture_version;
        }
        primitive->glyph_pages.push_back(SceneTextPrimitive::GlyphPageRenderable {
            .page_index = static_cast<uint32_t>(page_index),
            .texture_key = layout_page.texture_key,
            .source_size = layout_page.source_size,
            .mesh = BuildTextPrimitiveGlyphPageMesh(*primitive, static_cast<uint32_t>(page_index)),
        });
    }

    *out_primitive = std::move(primitive);
    return true;
}

bool wallpaper::SyncTextLayerSceneMaterials(Scene& scene, int32_t layer_id) {
    auto state_it = scene.textLayers.find(layer_id);
    if (state_it == scene.textLayers.end()) return false;
    auto& state = state_it->second;

    if (state.primitive == nullptr) {
        return false;
    }

    // The dedicated text pass consumes visual state directly from the scene-owned primitive.
    // Material-only updates therefore collapse to synchronizing the authored object onto that
    // primitive; no intermediate text nodes or secondary material replication steps are needed.
    state.primitive->object = state.object;
    return true;
}

bool wallpaper::RasterizeTextPrimitiveLayout(fs::VFS& vfs,
                                             wpscene::WPTextObject& object,
                                             const std::string& texture_key,
                                             double render_scale,
                                             TextRasterLayoutResult* out_image,
                                             std::string* out_error) {
    // Production text rasterization is deliberately silent; failures travel through `out_error`
    // so callers can decide how to surface hard rasterization errors.
    return GenerateTextLayoutImage(vfs, object, texture_key, render_scale, out_image, out_error);
}

bool wallpaper::UpdateTextLayerSceneTransform(Scene& scene, int32_t layer_id) {
    auto state_it = scene.textLayers.find(layer_id);
    if (state_it == scene.textLayers.end()) {
        return false;
    }

    auto&      state = state_it->second;
    const auto previous_geometry = CaptureTextLayerSceneGeometry(state);
    return SyncTextLayerSceneGeometry(scene, layer_id, state, previous_geometry);
}

bool wallpaper::ApplyTextLayerNodePlacement(SceneNode*                   node,
                                            const TextLayerRuntimeState& state,
                                            std::array<float, 3>         origin) {
    if (node == nullptr) return false;

    const auto alignment = ResolveTextLayerSceneAlignment(state.object);
    const auto placement_display_size = ResolveTextPlacementDisplaySize(state);
    const Eigen::Vector3f next_translation { origin[0], origin[1], origin[2] };
    const auto next_alignment_offset =
        ResolveTextPlacementLocalOffset(alignment, placement_display_size);
    const bool changed = !NearlyEqual(node->Translate(), next_translation) ||
                         !NearlyEqual(node->AlignmentOffset(), next_alignment_offset);

    if (changed) {
        node->SetTranslate(next_translation);
        node->SetAlignmentOffset(next_alignment_offset);
    }
    return changed;
}

bool wallpaper::ApplyTextLayerTransformValue(Scene&               scene,
                                             int32_t              layer_id,
                                             SceneNode*           node,
                                             std::string_view     property_name,
                                             std::array<float, 3> value) {
    auto state_it = scene.textLayers.find(layer_id);
    if (state_it == scene.textLayers.end()) return false;

    auto&      state = state_it->second;
    const auto previous_geometry = CaptureTextLayerSceneGeometry(state);
    if (!ApplyTextLayerObjectTransform(state, node, property_name, value)) return false;

    return SyncTextLayerSceneGeometry(scene, layer_id, state, previous_geometry);
}

bool wallpaper::ApplyTextLayerScreenAnchorTransforms(Scene& scene) {
    const auto frame = ResolveScreenAnchorFrame(scene);
    if (! frame.has_value()) {
        return false;
    }

    std::vector<ScreenAnchoredTextPlacement> placements;
    for (auto& [layer_id, state] : scene.textLayers) {
        if (scene.deferredRuntimeTextLayerIds.count(layer_id) != 0) continue;
        if (! HasExplicitTextScreenAnchor(state.object)) continue;

        const auto binding = scene.GetLayerParentBinding(layer_id);
        if (binding.parent_id != 0 || ! binding.attachment.empty()) {
            continue;
        }

        auto layer_node_it = scene.layerNodes.find(layer_id);
        if (layer_node_it == scene.layerNodes.end() || layer_node_it->second == nullptr) {
            continue;
        }

        SceneNode* layer_node = layer_node_it->second;
        const auto anchored_origin = ResolveScreenAnchoredTextOrigin(*frame, state.object);
        const Eigen::Vector3f next_translation {
            anchored_origin[0],
            anchored_origin[1],
            anchored_origin[2],
        };
        placements.push_back(ScreenAnchoredTextPlacement {
            .layer_id = layer_id,
            .state = &state,
            .node = layer_node,
            .translation = next_translation,
            .bounds = ResolveScreenAnchoredTextBounds(state, next_translation),
        });
        SnapScreenAnchoredTextPlacementToFrame(placements.back(), *frame);
    }

    // Resolve all screen-anchored text placements together before writing scene nodes. This keeps
    // same-corner HUD labels deterministic: each placement starts from the authored origin shifted
    // into the active camera frame, then only actual visible-glyph collisions introduce an extra
    // local stack offset.
    ResolveTopScreenAnchoredTextStack(placements);
    ResolveBottomScreenAnchoredTextStack(placements);

    bool changed_any_node = false;
    for (const auto& placement : placements) {
        if (placement.state == nullptr) {
            continue;
        }

        // This write is intentionally recomputed from the authored origin on every camera framing
        // update instead of accumulating deltas. Fill-mode changes can run every frame when camera
        // layers animate, and using the authored origin as the base keeps screen-anchored text
        // deterministic while preserving script-visible `thisLayer.origin` values.
        const bool changed = ApplyTextLayerNodePlacement(
            placement.node,
            *placement.state,
            { placement.translation.x(), placement.translation.y(), placement.translation.z() });
        if (!changed) {
            continue;
        }
        SyncTextLayerEffectTransform(scene, placement.layer_id, placement.node);
        changed_any_node = true;
    }

    return changed_any_node;
}

bool wallpaper::UpdateTextLayerSceneBridgeResources(Scene& scene, int32_t layer_id) {
    auto state_it = scene.textLayers.find(layer_id);
    if (state_it == scene.textLayers.end()) {
        return false;
    }

    auto& state = state_it->second;
    if (state.primitive == nullptr) {
        return false;
    }

    const auto previous_geometry = CaptureTextLayerSceneGeometry(state);
    // Bridge-resource updates are the permanent cheap path for geometry changes that do not alter
    // shaping results, such as toggling the opaque background. The primitive keeps the existing
    // atlas/layout data and only rebuilds the meshes whose local placement depends on the
    // canonical visible box before refreshing exact-size bridge targets and effect cameras.
    SyncTextPrimitiveCanonicalState(state, true);
    if (!ApplyTextLayerSceneGeometry(scene,
                                     layer_id,
                                     state,
                                     previous_geometry)) {
        return false;
    }

    // Geometry-only bridge edits can rebuild glyph/background meshes without changing any bridge
    // render-target size. Mark the owning text layer so its TextPass uploads those rebuilt buffers
    // during the next resource refresh instead of discovering them after frame recording starts.
    scene.MarkTextLayerResourcesDirty(layer_id);
    return true;
}

bool wallpaper::RebuildTextLayerSceneLayout(Scene& scene, int32_t layer_id) {
    if (scene.deferredRuntimeTextLayerIds.count(layer_id) != 0) {
        return true;
    }

    auto state_it = scene.textLayers.find(layer_id);
    if (state_it == scene.textLayers.end() || scene.vfs == nullptr) {
        return false;
    }

    auto&      state                 = state_it->second;
    const auto previous_geometry = CaptureTextLayerSceneGeometry(state);
    // Atlas revisions now live on the scene-owned primitive itself. Runtime layout rebuilds only
    // need the next monotonically increasing atlas version so the dedicated text pass refreshes
    // page textures after a reraster; keeping that counter on the primitive removes another piece
    // of duplicate runtime bookkeeping from the layer registry entry.
    const auto next_texture_version =
        state.primitive != nullptr ? state.primitive->atlas_version + 1u : 1u;

    std::string error;
    std::shared_ptr<SceneTextPrimitive> rebuilt_primitive;
    if (!BuildSceneTextPrimitive(*scene.vfs,
                                 state.object,
                                 next_texture_version,
                                 scene.textRenderScale,
                                 &rebuilt_primitive,
                                 &error)) {
        return false;
    }

    // The parser is no longer the authority for live text content. Runtime rerenders rebuild a
    // fresh scene-owned text primitive and then swap that primitive onto the existing scene nodes,
    // which keeps the dedicated text pass on a single canonical representation at both parse time
    // and runtime.
    if (state.primitive != nullptr) {
        rebuilt_primitive->bridge = state.primitive->bridge;
    }
    rebuilt_primitive->bridge.source_size = rebuilt_primitive->VisibleSourceSize();

    auto primitive_nodes = FindTextPrimitiveRuntimeNodes(scene, layer_id);
    if (primitive_nodes.empty()) {
        return false;
    }
    for (auto* primitive_node : primitive_nodes) {
        primitive_node->AddText(rebuilt_primitive);
    }

    state.primitive = rebuilt_primitive;
    state.object = rebuilt_primitive->object;
    SyncTextPrimitiveCanonicalState(state, false);

    if (!ApplyTextLayerSceneGeometry(scene,
                                     layer_id,
                                     state,
                                     previous_geometry)) {
        return false;
    }

    // Runtime layout rerasters replace atlas pages and glyph meshes even for direct-to-screen text
    // such as Clock. The render graph topology is unchanged, but the pass-owned GPU resources must
    // refresh before the next draw so atlas descriptors and vertex/index buffers stay frame-coherent.
    scene.MarkTextLayerResourcesDirty(layer_id);
    return true;
}
