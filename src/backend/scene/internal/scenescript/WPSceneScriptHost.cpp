#include "WPSceneScriptHost.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iomanip>
#include <memory>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "scene/Scene.h"
#include "scene/SceneImageEffectLayer.h"
#include "scene/SceneMaterial.h"
#include "scene/SceneMesh.h"
#include "scene/SceneNode.h"
#include "scene/SceneTextPrimitive.h"
#include "audio/SoundManager.h"
#include "fs/VFS.h"
#include "particle/ParticleSystem.h"
#include "SpecTexs.hpp"
#include "utils/Logging.h"
#include "WPJson.hpp"
#include "WPDynamicValue.hpp"
#include "WPImageAlignment.hpp"
#include "WPSceneParser.hpp"
#include "WPSceneScriptMedia.hpp"
#include "WPSyntheticImageParser.hpp"
#include "WPShaderValueUpdater.hpp"
#include "WPTextLayer.hpp"

extern "C" {
#include "quickjs.h"
}

namespace wallpaper
{

struct SceneRegistrationRange {
    std::size_t binding_start { 0 };
    std::size_t binding_end { std::numeric_limits<std::size_t>::max() };
    std::size_t animation_start { 0 };
    std::size_t animation_end { std::numeric_limits<std::size_t>::max() };
    std::size_t script_start { 0 };
    std::size_t script_end { std::numeric_limits<std::size_t>::max() };
};

namespace
{

std::string FormatVec2ForLog(const std::array<float, 2>& value) {
    std::ostringstream oss;
    oss << "[" << std::fixed << std::setprecision(3) << value[0] << ", " << value[1] << "]";
    return oss.str();
}

std::string FormatVec3ForLog(const std::array<float, 3>& value) {
    std::ostringstream oss;
    oss << "[" << std::fixed << std::setprecision(3) << value[0] << ", " << value[1] << ", "
        << value[2] << "]";
    return oss.str();
}

std::string FormatEigenVec3ForLog(const Eigen::Vector3f& value) {
    std::ostringstream oss;
    oss << "[" << std::fixed << std::setprecision(3) << value.x() << ", " << value.y() << ", "
        << value.z() << "]";
    return oss.str();
}

std::string FormatStringListForLog(const std::vector<std::string>& values) {
    std::ostringstream oss;
    oss << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) oss << ", ";
        oss << values[index];
    }
    oss << "]";
    return oss.str();
}

void LogSceneLayer332State(const Scene& scene) {
    constexpr int32_t layer_id = 332;
    const auto node_it = scene.layerNodes.find(layer_id);
    const auto state_it = scene.imageLayers.find(layer_id);
    const auto runtime_targets_it = scene.objectRuntimeRenderTargets.find(layer_id);
    const auto runtime_cameras_it = scene.objectRuntimeCameraNames.find(layer_id);
    if (node_it == scene.layerNodes.end() || state_it == scene.imageLayers.end()) return;

    const SceneNode* node = node_it->second;
    LOG_INFO("DebugLayer332Runtime: node-translate=%s node-scale=%s node-align=%s image-size=%s "
             "image-alignment='%s' cameras=%s targets=%s",
             node != nullptr ? FormatEigenVec3ForLog(node->Translate()).c_str() : "null",
             node != nullptr ? FormatEigenVec3ForLog(node->Scale()).c_str() : "null",
             node != nullptr ? FormatEigenVec3ForLog(node->AlignmentOffset()).c_str() : "null",
             FormatVec2ForLog(state_it->second.size).c_str(),
             state_it->second.alignment.c_str(),
             runtime_cameras_it != scene.objectRuntimeCameraNames.end()
                 ? FormatStringListForLog(runtime_cameras_it->second).c_str()
                 : "[]",
             runtime_targets_it != scene.objectRuntimeRenderTargets.end()
                 ? FormatStringListForLog(runtime_targets_it->second).c_str()
                 : "[]");
}

void LogSceneTextLayerState(const Scene& scene, int32_t layer_id, const char* label) {
    const auto text_it = scene.textLayers.find(layer_id);
    const auto node_it = scene.layerNodes.find(layer_id);
    if (text_it == scene.textLayers.end() || node_it == scene.layerNodes.end()) return;

    const auto&     state = text_it->second;
    const SceneNode* node = node_it->second;
    const auto runtime_targets_it = scene.objectRuntimeRenderTargets.find(layer_id);
    const auto runtime_cameras_it = scene.objectRuntimeCameraNames.find(layer_id);
    const auto visible_display_size =
        state.primitive != nullptr ? state.primitive->VisibleDisplaySize() : std::array<float, 2> {};
    const auto visible_source_size =
        state.primitive != nullptr ? state.primitive->VisibleSourceSize() : std::array<float, 2> {};
    const auto visible_display_offset = state.primitive != nullptr
        ? state.primitive->VisibleDisplayOffset()
        : std::array<float, 2> {};

    LOG_INFO("Debug%sRuntime: object-origin=%s object-scale=%s object-angles=%s anchor='%s' "
             "copybackground=%s node-translate=%s node-scale=%s node-align=%s "
             "visible-display=%s visible-source=%s visible-offset=%s cameras=%s targets=%s",
             label,
             FormatVec3ForLog(state.object.origin).c_str(),
             FormatVec3ForLog(state.object.scale).c_str(),
             FormatVec3ForLog(state.object.angles).c_str(),
             state.object.anchor.c_str(),
             state.object.copybackground ? "true" : "false",
             node != nullptr ? FormatEigenVec3ForLog(node->Translate()).c_str() : "null",
             node != nullptr ? FormatEigenVec3ForLog(node->Scale()).c_str() : "null",
             node != nullptr ? FormatEigenVec3ForLog(node->AlignmentOffset()).c_str() : "null",
             FormatVec2ForLog(visible_display_size).c_str(),
             FormatVec2ForLog(visible_source_size).c_str(),
             FormatVec2ForLog(visible_display_offset).c_str(),
             runtime_cameras_it != scene.objectRuntimeCameraNames.end()
                 ? FormatStringListForLog(runtime_cameras_it->second).c_str()
                 : "[]",
             runtime_targets_it != scene.objectRuntimeRenderTargets.end()
                 ? FormatStringListForLog(runtime_targets_it->second).c_str()
                 : "[]");
}

struct TextureAnimationState {
    SpriteAnimation base_animation;
    SpriteAnimation animation;
    double          rate { 1.0 };
};

struct AnimationLayerRuntimeState {
    double               last_time { 0.0 };
    bool                 seen { false };
    std::vector<JSValue> ended_callbacks;
};

struct PropertyAnimationInstance {
    WPSceneScriptRegistration registration;
    uint32_t                  animation_id { 0 };
    WPPropertyAnimationState  state;
};

struct ScriptInstance {
    uint32_t                  instance_id { 0 };
    WPSceneScriptRegistration registration;
    WPDynamicValue            current_value;
    JSValue                   script_properties { JS_UNDEFINED };
    JSValue                   exports { JS_UNDEFINED };
    JSValue                   init_fn { JS_UNDEFINED };
    JSValue                   update_fn { JS_UNDEFINED };
    JSValue                   apply_user_properties_fn { JS_UNDEFINED };
    JSValue                   apply_general_settings_fn { JS_UNDEFINED };
    JSValue                   cursor_enter_fn { JS_UNDEFINED };
    JSValue                   cursor_leave_fn { JS_UNDEFINED };
    JSValue                   cursor_move_fn { JS_UNDEFINED };
    JSValue                   cursor_down_fn { JS_UNDEFINED };
    JSValue                   cursor_up_fn { JS_UNDEFINED };
    JSValue                   cursor_click_fn { JS_UNDEFINED };
    JSValue                   media_thumbnail_changed_fn { JS_UNDEFINED };
    JSValue                   media_properties_changed_fn { JS_UNDEFINED };
    JSValue                   media_playback_changed_fn { JS_UNDEFINED };
    JSValue                   destroy_fn { JS_UNDEFINED };
    JSValue                   resize_screen_fn { JS_UNDEFINED };
    bool                      initialized { false };
};

struct CursorPositionState {
    double screen_x { 0.0 };
    double screen_y { 0.0 };
    double world_x { 0.0 };
    double world_y { 0.0 };
};

struct ScriptTimer {
    uint64_t id { 0 };
    uint32_t owner_instance_id { 0 };
    double   remaining_ms { 0.0 };
    double   interval_ms { 0.0 };
    bool     repeat { false };
    JSValue  callback { JS_UNDEFINED };
};

struct AudioBufferBinding {
    uint32_t resolution { 0 };
    JSValue  object { JS_UNDEFINED };
    JSValue  left { JS_UNDEFINED };
    JSValue  right { JS_UNDEFINED };
    JSValue  average { JS_UNDEFINED };
};

struct SpectrumBucketStats {
    size_t begin { 0 };
    size_t end { 0 };
    float  average { 0.0f };
    float  peak { 0.0f };
};

constexpr int    kDetailedExternalAudioLogLimit = 6;
std::atomic<int> g_detailed_external_audio_logs_remaining { kDetailedExternalAudioLogLimit };

bool ShouldLogDetailedExternalAudio(uint32_t resolution) {
    if (resolution != 16) return false;

    int remaining = g_detailed_external_audio_logs_remaining.load();
    while (remaining > 0) {
        if (g_detailed_external_audio_logs_remaining.compare_exchange_weak(remaining,
                                                                           remaining - 1))
            return true;
    }
    return false;
}

std::vector<float> ResampleSpectrumChannel(const std::vector<float>& values, uint32_t resolution) {
    const size_t       target_size = static_cast<size_t>(resolution);
    std::vector<float> result(target_size, 0.0f);
    if (target_size == 0 || values.empty()) return result;

    const size_t source_size = values.size();
    if (target_size == source_size) return values;
    for (size_t i = 0; i < target_size; i++) {
        const double source_position =
            ((static_cast<double>(i) + 0.5) * static_cast<double>(source_size) /
             static_cast<double>(target_size)) -
            0.5;
        const double clamped_position =
            std::clamp(source_position, 0.0, static_cast<double>(source_size - 1));
        const size_t lower_index = static_cast<size_t>(std::floor(clamped_position));
        const size_t upper_index = std::min(source_size - 1, lower_index + 1);
        const float  mix = static_cast<float>(clamped_position - static_cast<double>(lower_index));
        const float  lower_value = values[lower_index];
        const float  upper_value = values[upper_index];
        result[i]                = lower_value + (upper_value - lower_value) * mix;
    }
    return result;
}

float NormalizeExternalSpectrumValue(float value) {
    if (! std::isfinite(value) || value <= 0.0f) return 0.0f;

    // Renderer-provided external audio already arrives as normalized spectrum energy.
    // Preserve that shape here instead of re-scaling/clamping it into a flat 0..2 plateau.
    return std::clamp(value, 0.0f, 2.0f);
}

std::vector<float> NormalizeExternalSpectrumChannel(const std::vector<float>& values) {
    std::vector<float> normalized(values.size(), 0.0f);
    for (size_t i = 0; i < values.size(); i++)
        normalized[i] = NormalizeExternalSpectrumValue(values[i]);
    return normalized;
}

std::string FormatSpectrumSlice(const std::vector<float>& values, size_t count = 8) {
    std::ostringstream stream;
    stream << "[";
    const auto limit = std::min(values.size(), count);
    for (size_t i = 0; i < limit; i++) {
        if (i > 0) stream << ", ";
        stream << std::fixed << std::setprecision(4) << values[i];
    }
    if (values.size() > limit) stream << ", ...";
    stream << "]";
    return stream.str();
}

std::string FormatNormalizationPairs(const std::vector<float>& source,
                                     const std::vector<float>& normalized, size_t count = 8) {
    std::ostringstream stream;
    stream << "[";
    const auto limit = std::min({ source.size(), normalized.size(), count });
    for (size_t i = 0; i < limit; i++) {
        if (i > 0) stream << ", ";
        stream << std::fixed << std::setprecision(4) << source[i] << "->" << normalized[i];
    }
    if (std::min(source.size(), normalized.size()) > limit) stream << ", ...";
    stream << "]";
    return stream.str();
}

std::vector<SpectrumBucketStats> ComputeSpectrumBucketStats(const std::vector<float>& values,
                                                            uint32_t                  resolution) {
    const size_t                     target_size = static_cast<size_t>(resolution);
    std::vector<SpectrumBucketStats> stats(target_size);
    if (target_size == 0 || values.empty()) return stats;

    const size_t source_size = values.size();
    if (target_size >= source_size) {
        for (size_t i = 0; i < target_size; i++) {
            const size_t source_index = std::min(source_size - 1, (i * source_size) / target_size);
            stats[i]                  = {
                source_index,
                std::min(source_size, source_index + 1),
                values[source_index],
                values[source_index],
            };
        }
        return stats;
    }

    for (size_t i = 0; i < target_size; i++) {
        const size_t begin       = (i * source_size) / target_size;
        const size_t end         = std::max(begin + 1, ((i + 1) * source_size) / target_size);
        const size_t clamped_end = std::min(end, source_size);
        float        sum { 0.0f };
        float        peak { 0.0f };
        for (size_t j = begin; j < clamped_end; j++) {
            sum += values[j];
            peak = std::max(peak, values[j]);
        }
        const auto count = static_cast<float>(std::max<size_t>(1, clamped_end - begin));
        stats[i]         = {
            begin,
            clamped_end,
            sum / count,
            peak,
        };
    }
    return stats;
}

std::string FormatSpectrumBucketStats(const std::vector<SpectrumBucketStats>& stats,
                                      size_t                                  count = 8) {
    std::ostringstream stream;
    stream << "[";
    const auto limit = std::min(stats.size(), count);
    for (size_t i = 0; i < limit; i++) {
        if (i > 0) stream << ", ";
        stream << i << ":" << stats[i].begin << "-" << stats[i].end << " avg=" << std::fixed
               << std::setprecision(4) << stats[i].average << " peak=" << stats[i].peak;
    }
    if (stats.size() > limit) stream << ", ...";
    stream << "]";
    return stream.str();
}

struct RuntimeState {
    JSRuntime* runtime { nullptr };
    JSContext* context { nullptr };
};

enum class LocalStorageLocation;

struct LayerValueHint {
    WPDynamicValue::Type type { WPDynamicValue::Type::Null };
    bool                 supported { false };
};

struct NodeScaleSnapshot {
    Eigen::Vector3f local { 1.0f, 1.0f, 1.0f };
    Eigen::Vector3f world { 1.0f, 1.0f, 1.0f };
};

SceneNode* FindNodeById(WPSceneScriptHost::Opaque* opaque, int32_t node_id);
int32_t    FindNodeId(const WPSceneScriptHost::Opaque* opaque, const SceneNode* node);
bool       ApplyLayerPropertyValue(WPSceneScriptHost::Opaque* opaque, SceneNode* node,
                                   std::string_view property_name, const WPDynamicValue& value);
void       UpdateJSNumericArray(JSContext* context, JSValueConst target,
                                const std::vector<float>& values);
JSValue    CreateJSFloat32Array(JSContext* context, const std::vector<float>& values);
bool       UpdateJSFloat32Array(JSContext* context, JSValueConst target,
                                const std::vector<float>& values);
void       UpdateAudioBufferBindings(WPSceneScriptHost::Opaque* opaque);
void       UpdatePropertyAnimations(WPSceneScriptHost::Opaque* opaque, double frame_time);
CursorPositionState ComputeCursorPositionState(const WPSceneScriptHost::Opaque* opaque);
nlohmann::json*     ResolveLocalStorageBucket(WPSceneScriptHost::Opaque* opaque,
                                              LocalStorageLocation       location);
void UpdateGeneralSettingsObject(JSContext* context, JSValueConst target,
                                 const std::unordered_map<std::string, std::string>& settings,
                                 std::unordered_set<std::string>&                    known_names);
void FreeJSValue(JSContext* context, JSValue& value);
bool ApplyPropertyAnimationInstance(WPSceneScriptHost::Opaque* opaque,
                                    PropertyAnimationInstance& animation);
bool ApplyRegistrationValue(WPSceneScriptHost::Opaque*       opaque,
                            const WPSceneScriptRegistration& registration,
                            const WPDynamicValue&            value);
void RebindLayerRegistrations(WPSceneScriptHost::Opaque* opaque, int32_t layer_id, SceneNode* node);
void RegisterSceneRegistrationRange(WPSceneScriptHost::Opaque* opaque,
                                    const SceneRegistrationRange& range);
bool MaterializeDeferredImageLayerIfNeeded(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                                           bool mark_graph_dirty = true);
bool MaterializeDeferredParticleLayerIfNeeded(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                                              bool mark_graph_dirty = true);
bool MaterializeDeferredTextLayerIfNeeded(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                                          bool mark_graph_dirty = true);
bool MaterializeDeferredVisibleLayerTreeIfNeeded(WPSceneScriptHost::Opaque* opaque,
                                                 int32_t                    root_layer_id);
void QueueHiddenLayerTreeResourceRelease(WPSceneScriptHost::Opaque* opaque,
                                         int32_t                    root_layer_id);
bool IsDeferredRuntimeLayer(const WPSceneScriptHost::Opaque* opaque, int32_t layer_id);
void EnsureTextureAnimationStatesForNode(WPSceneScriptHost::Opaque* opaque, SceneNode* node);
bool RunScriptInstanceInit(WPSceneScriptHost::Opaque* opaque, ScriptInstance& instance);
void ResortLayerTree(SceneNode* parent, const WPSceneScriptHost::Opaque* opaque);
std::optional<Eigen::Matrix4d> GetAttachmentWorldTransform(const WPSceneScriptHost::Opaque* opaque,
                                                           SceneNode*                       node,
                                                           const WPPuppet::Attachment& attachment);
std::string DescribeScriptInstance(const WPSceneScriptHost::Opaque* opaque, uint32_t instance_id);
std::string NormalizeLocaleToWallpaperLanguage(std::string locale_name);

LayerValueHint LayerValueType(std::string_view property_name) {
    if (property_name == "visible") return { WPDynamicValue::Type::Boolean, true };
    if (property_name == "origin") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "angles") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "scale") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "parallaxDepth") return { WPDynamicValue::Type::Float2, true };
    if (property_name == "size") return { WPDynamicValue::Type::Float2, true };
    if (property_name == "text") return { WPDynamicValue::Type::String, true };
    if (property_name == "font") return { WPDynamicValue::Type::String, true };
    if (property_name == "color") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "colorn") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "rate") return { WPDynamicValue::Type::Float, true };
    if (property_name == "alpha") return { WPDynamicValue::Type::Float, true };
    if (property_name == "brightness") return { WPDynamicValue::Type::Float, true };
    if (property_name == "backgroundcolor") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "backgroundbrightness") return { WPDynamicValue::Type::Float, true };
    if (property_name == "opaquebackground") return { WPDynamicValue::Type::Boolean, true };
    if (property_name == "pointsize") return { WPDynamicValue::Type::Float, true };
    if (property_name == "padding") return { WPDynamicValue::Type::Int32, true };
    if (property_name == "horizontalalign") return { WPDynamicValue::Type::String, true };
    if (property_name == "verticalalign") return { WPDynamicValue::Type::String, true };
    if (property_name == "anchor") return { WPDynamicValue::Type::String, true };
    if (property_name == "limitrows") return { WPDynamicValue::Type::Boolean, true };
    if (property_name == "maxrows") return { WPDynamicValue::Type::Int32, true };
    if (property_name == "limitwidth") return { WPDynamicValue::Type::Boolean, true };
    if (property_name == "maxwidth") return { WPDynamicValue::Type::Float, true };
    return {};
}

LayerValueHint CameraLayerValueType(std::string_view property_name) {
    if (property_name == "visible") return { WPDynamicValue::Type::Boolean, true };
    if (property_name == "origin") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "angles") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "zoom") return { WPDynamicValue::Type::Float, true };
    if (property_name == "fov") return { WPDynamicValue::Type::Float, true };
    return {};
}

LayerValueHint AnimationLayerValueType(std::string_view property_name) {
    if (property_name == "visible") return { WPDynamicValue::Type::Boolean, true };
    if (property_name == "rate") return { WPDynamicValue::Type::Float, true };
    if (property_name == "blend") return { WPDynamicValue::Type::Float, true };
    return {};
}

LayerValueHint EffectValueType(std::string_view property_name) {
    // This visibility refactor intentionally promotes only effect.visible to a runtime target.
    // Effect material constant scripts are logged by the parser as unsupported so they do not get
    // silently misrouted to the owning layer.
    if (property_name == "visible") return { WPDynamicValue::Type::Boolean, true };
    return {};
}

LayerValueHint SoundValueType(std::string_view property_name) {
    // Sound-layer user bindings currently expose only the stream gain. Playback commands remain
    // script methods because they mutate channel state rather than a scalar project property.
    if (property_name == "volume") return { WPDynamicValue::Type::Float, true };
    return {};
}

bool IsParticleColorProperty(std::string_view property_name) {
    return property_name == "colorn" || property_name == "color";
}

bool IsParticleSizeProperty(std::string_view property_name) {
    return property_name == "size";
}

bool IsParticleRateProperty(std::string_view property_name) {
    // Nested particle scripts are registered as layer targets with the authored override name.
    // Keeping the name plain `rate` lets the existing script/user-property dispatcher stay shared
    // while ApplyParticlePropertyValue() routes it to ParticleSubSystem instead of image layers.
    return property_name == "rate";
}

std::optional<std::array<float, 3>> NormalizeParticleColorValue(
    std::string_view property_name, const WPDynamicValue& value) {
    std::array<float, 3> color {};
    if (!value.tryGet(&color)) return std::nullopt;

    if (property_name == "color") {
        const bool authored_as_byte_color = std::any_of(
            color.begin(), color.end(), [](float channel) { return std::abs(channel) > 1.0f; });
        if (authored_as_byte_color) {
            // Wallpaper particle `instanceoverride.color` is the byte-color variant consumed by
            // genOverrideInitOp(), while `instanceoverride.colorn` is already normalized. User
            // color pickers usually send normalized values, so only scale the byte-style payloads.
            for (auto& channel : color) channel /= 255.0f;
        }
    }

    return color;
}

std::optional<float> NormalizeParticleSizeValue(const WPDynamicValue& value) {
    float size = 0.0f;
    if (!value.tryGet(&size) || !std::isfinite(size)) return std::nullopt;
    return size;
}

std::optional<float> NormalizeParticleRateValue(const WPDynamicValue& value) {
    float rate = 0.0f;
    if (!value.tryGet(&rate) || !std::isfinite(rate)) return std::nullopt;

    // Negative particle time would make lifetime decay and emitter timers run backward, which is
    // not a supported Wallpaper particle override. Clamp live script output to the same forward
    // clock domain used by property-animation rate setters elsewhere in the runtime.
    return std::max(0.0f, rate);
}

const char* TargetKindName(WPSceneScriptTargetKind target_kind) {
    switch (target_kind) {
    case WPSceneScriptTargetKind::Scene: return "scene";
    case WPSceneScriptTargetKind::Sound: return "sound";
    case WPSceneScriptTargetKind::Camera: return "camera";
    case WPSceneScriptTargetKind::Layer: return "layer";
    case WPSceneScriptTargetKind::AnimationLayer: return "animationLayer";
    case WPSceneScriptTargetKind::Effect: return "effect";
    case WPSceneScriptTargetKind::MaterialUniform: return "materialUniform";
    }
    return "layer";
}

bool IsOpacityUniformName(std::string_view property_name) {
    return property_name == "alpha" || property_name == "g_Alpha" ||
           property_name == "g_UserAlpha";
}

bool IsOpacityRegistration(const WPSceneScriptRegistration& registration) {
    if (registration.target_kind == WPSceneScriptTargetKind::Layer) {
        return registration.property_name == "alpha";
    }
    if (registration.target_kind == WPSceneScriptTargetKind::MaterialUniform) {
        return IsOpacityUniformName(registration.property_name);
    }
    return false;
}

float ClampOpacityScalar(float opacity) {
    if (! std::isfinite(opacity)) return 0.0f;
    return std::clamp(opacity, 0.0f, 1.0f);
}

WPDynamicValue ClampOpacityRegistrationValue(const WPSceneScriptRegistration& registration,
                                             const WPDynamicValue&            value) {
    if (! IsOpacityRegistration(registration)) return value;

    float opacity = 0.0f;
    if (! value.tryGet(&opacity)) return value;

    // Opacity registrations are shader alpha inputs with a normalized 0..1 output domain. Keep
    // this clamp at the registration boundary so authored Bezier overshoot remains available to
    // scale, origin, color, and other non-opacity animation targets.
    return WPDynamicValue(ClampOpacityScalar(opacity));
}

LayerValueHint SceneValueType(std::string_view property_name) {
    if (property_name == "clearcolor") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "ambientcolor") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "skylightcolor") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "bloom") return { WPDynamicValue::Type::Boolean, true };
    if (property_name == "bloomstrength") return { WPDynamicValue::Type::Float, true };
    if (property_name == "bloomthreshold") return { WPDynamicValue::Type::Float, true };
    if (property_name == "bloomtint") return { WPDynamicValue::Type::Float3, true };
    if (property_name == "cameraparallax") return { WPDynamicValue::Type::Boolean, true };
    if (property_name == "cameraparallaxamount") return { WPDynamicValue::Type::Float, true };
    if (property_name == "cameraparallaxdelay") return { WPDynamicValue::Type::Float, true };
    if (property_name == "cameraparallaxmouseinfluence")
        return { WPDynamicValue::Type::Float, true };
    if (property_name == "fov") return { WPDynamicValue::Type::Float, true };
    if (property_name == "nearz") return { WPDynamicValue::Type::Float, true };
    if (property_name == "farz") return { WPDynamicValue::Type::Float, true };
    return {};
}

std::string DescribeDynamicValueForLog(const WPDynamicValue& value) {
    std::ostringstream stream;
    switch (value.type()) {
    case WPDynamicValue::Type::Null: return "null";
    case WPDynamicValue::Type::Boolean: {
        bool boolean_value = false;
        value.tryGet(&boolean_value);
        return boolean_value ? "bool(true)" : "bool(false)";
    }
    case WPDynamicValue::Type::Int32: {
        int32_t int_value = 0;
        value.tryGet(&int_value);
        stream << "int(" << int_value << ")";
        return stream.str();
    }
    case WPDynamicValue::Type::UInt32: {
        uint32_t uint_value = 0;
        value.tryGet(&uint_value);
        stream << "uint(" << uint_value << ")";
        return stream.str();
    }
    case WPDynamicValue::Type::Float: {
        float float_value = 0.0f;
        value.tryGet(&float_value);
        stream << "float(" << float_value << ")";
        return stream.str();
    }
    case WPDynamicValue::Type::Double: {
        double double_value = 0.0;
        value.tryGet(&double_value);
        stream << "double(" << double_value << ")";
        return stream.str();
    }
    case WPDynamicValue::Type::String: {
        std::string string_value;
        value.tryGet(&string_value);
        stream << "string(\"" << string_value << "\")";
        return stream.str();
    }
    case WPDynamicValue::Type::FloatVector: {
        std::vector<float> vector_value;
        value.tryGet(&vector_value);
        stream << "floatvec(";
        for (size_t index = 0; index < vector_value.size(); index++) {
            if (index != 0) stream << ", ";
            stream << vector_value[index];
        }
        stream << ")";
        return stream.str();
    }
    case WPDynamicValue::Type::Int3: {
        std::array<int32_t, 3> array_value {};
        value.tryGet(&array_value);
        stream << "int3(" << array_value[0] << ", " << array_value[1] << ", " << array_value[2]
               << ")";
        return stream.str();
    }
    case WPDynamicValue::Type::Float2: {
        std::array<float, 2> array_value {};
        value.tryGet(&array_value);
        stream << "float2(" << array_value[0] << ", " << array_value[1] << ")";
        return stream.str();
    }
    case WPDynamicValue::Type::Float3: {
        std::array<float, 3> array_value {};
        value.tryGet(&array_value);
        stream << "float3(" << array_value[0] << ", " << array_value[1] << ", " << array_value[2]
               << ")";
        return stream.str();
    }
    case WPDynamicValue::Type::Float4: {
        std::array<float, 4> array_value {};
        value.tryGet(&array_value);
        stream << "float4(" << array_value[0] << ", " << array_value[1] << ", " << array_value[2]
               << ", " << array_value[3] << ")";
        return stream.str();
    }
    }

    return "<unknown>";
}

std::string DescribeTrackedUserPropertyForLog(const UserPropertyMap& properties,
                                              std::string_view       name) {
    const auto* entry = FindUserPropertyEntry(&properties, name);
    if (entry == nullptr) return "<missing>";

    if (const auto* shader_value = std::get_if<ShaderValue>(&entry->value)) {
        std::ostringstream out;
        out << "shader(";
        for (size_t index = 0; index < shader_value->size(); index++) {
            if (index != 0) out << ' ';
            out << (*shader_value)[index];
        }
        out << ")";
        return out.str();
    }

    return std::string("string(\"") + std::get<std::string>(entry->value) + "\")";
}

std::string DescribeUserPropertyEntryForLog(const UserProperty* entry) {
    if (entry == nullptr) return "<missing>";

    std::ostringstream out;
    if (const auto* shader_value = std::get_if<ShaderValue>(&entry->value)) {
        out << "shader(";
        for (size_t index = 0; index < shader_value->size(); index++) {
            if (index != 0) out << ' ';
            out << (*shader_value)[index];
        }
        out << ")";
    } else {
        out << "string(\"" << std::get<std::string>(entry->value) << "\")";
    }

    out << " condition=\"" << entry->condition << "\""
        << " is-boolean=" << (entry->is_boolean ? "true" : "false");
    return out.str();
}

enum class LocalStorageLocation
{
    Screen,
    Global,
};

LocalStorageLocation ResolveLocalStorageLocation(std::string_view location_name) {
    return location_name == "global" ? LocalStorageLocation::Global : LocalStorageLocation::Screen;
}

std::string NormalizeLocaleToWallpaperLanguage(std::string locale_name) {
    if (locale_name.empty()) return "en-us";

    const auto dot_pos = locale_name.find('.');
    if (dot_pos != std::string::npos) locale_name.erase(dot_pos);
    const auto at_pos = locale_name.find('@');
    if (at_pos != std::string::npos) locale_name.erase(at_pos);

    for (char& ch : locale_name) {
        if (ch == '_') {
            ch = '-';
        } else {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }

    if (locale_name.empty() || locale_name == "c" || locale_name == "posix") {
        return "en-us";
    }

    if (locale_name == "zh" || locale_name == "zh-cn" || locale_name == "zh-sg") {
        return "zh-chs";
    }
    if (locale_name == "zh-tw" || locale_name == "zh-hk" || locale_name == "zh-mo") {
        return "zh-cht";
    }
    if (locale_name == "en") return "en-us";

    if (locale_name.find('-') == std::string::npos && locale_name.size() == 2) {
        return locale_name + "-" + locale_name;
    }

    return locale_name;
}

std::unordered_map<std::string, std::string> BuildInitialGeneralSettings() {
    const char* locale_name = std::getenv("LC_ALL");
    if (locale_name == nullptr || locale_name[0] == '\0') locale_name = std::getenv("LC_MESSAGES");
    if (locale_name == nullptr || locale_name[0] == '\0') locale_name = std::getenv("LANG");

    return {
        { "language",
          NormalizeLocaleToWallpaperLanguage(locale_name != nullptr ? locale_name : "") },
    };
}

void ReplaceAll(std::string& source, std::string_view needle, std::string_view replacement) {
    std::string::size_type pos = 0;
    while ((pos = source.find(needle, pos)) != std::string::npos) {
        source.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

void NormalizeScriptWhitespace(std::string& source) {
    // Workshop scripts can contain U+00A0 non-breaking spaces around module
    // syntax copied from localized snippets. QuickJS parses those as
    // whitespace, while the local compatibility pass used to miss
    // `export\xc2\xa0function` and `export\xc2\xa0var`. Normalize before
    // stripping so persistent runtime callbacks compile the same scripts that
    // Wallpaper Engine accepts.
    ReplaceAll(source, "\xC2\xA0", " ");
}

std::string StripScriptModuleSyntax(std::string source) {
    NormalizeScriptWhitespace(source);

    std::string::size_type pos = 0;
    while ((pos = source.find("'use strict';", pos)) != std::string::npos) {
        source.erase(pos, 13);
    }

    pos = 0;
    while ((pos = source.find("\"use strict\";", pos)) != std::string::npos) {
        source.erase(pos, 13);
    }

    pos = 0;
    while ((pos = source.find("export ", pos)) != std::string::npos) {
        source.erase(pos, 7);
    }

    std::istringstream input(source);
    std::ostringstream output;
    std::string        line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t");
        const auto trimmed =
            first == std::string::npos ? std::string_view {} : std::string_view(line).substr(first);
        if (trimmed.starts_with("import ")) {
            continue;
        }
        output << line << '\n';
    }

    return output.str();
}

std::string BuildPersistentScript(std::string_view script_source) {
    const std::string body = StripScriptModuleSyntax(std::string(script_source));

    std::ostringstream wrapper;
    wrapper
        << "(function(__env) {\n"
        << "  const __native = __env.native;\n"
        << "  const __instanceId = __env.instanceId;\n"
        << "  const __nodeId = __env.nodeId;\n"
        << "  const __objectKind = __env.objectKind;\n"
        << "  const __objectIndex = __env.objectIndex;\n"
        << "  const __propertyName = __env.propertyName;\n"
        << "  const __props = __env.scriptProperties;\n"
        << "  const __sharedTarget = (__env.shared && typeof __env.shared === 'object') ? "
           "__env.shared : {};\n"
        << "  Object.defineProperty(globalThis, 'shared', {\n"
        << "    configurable: true,\n"
        << "    enumerable: true,\n"
        << "    get() {\n"
        << "      return __sharedTarget;\n"
        << "    },\n"
        << "    set(value) {\n"
        << "      const nextValue = value && typeof value === 'object' ? value : {};\n"
        << "      for (const key of Object.keys(__sharedTarget)) {\n"
        << "        delete __sharedTarget[key];\n"
        << "      }\n"
        << "      Object.assign(__sharedTarget, nextValue);\n"
        << "    }\n"
        << "  });\n"
        << "  const console = __env.console;\n"
        << "  const input = __env.input;\n"
        << "  const generalSettings = __env.generalSettings;\n"
        << "  const engine = Object.create(__env.engineBase);\n"
        << "  engine.userProperties = __env.engineBase.userProperties;\n"
        << "  Object.defineProperty(engine, 'canvasSize', {\n"
        << "    configurable: true,\n"
        << "    enumerable: true,\n"
        << "    get() {\n"
        << "      const value = __env.engineBase.canvasSize;\n"
        << "      return value && typeof value.divide === 'function' ? value : new Vec2(value);\n"
        << "    }\n"
        << "  });\n"
        << "  Object.defineProperty(engine, 'screenResolution', {\n"
        << "    configurable: true,\n"
        << "    enumerable: true,\n"
        << "    get() {\n"
        << "      const value = __env.engineBase.screenResolution;\n"
        << "      return value && typeof value.divide === 'function' ? value : new Vec2(value);\n"
        << "    }\n"
        << "  });\n"
        << "  // Wallpaper Engine exposes media playback states as a global enum object. Define "
           "the\n"
        << "  // values inside every script wrapper as well so stripped module scripts can "
           "reference\n"
        << "  // MediaPlaybackEvent.PLAYBACK_* without depending on evaluation order between "
           "layers.\n"
        << "  const MediaPlaybackEvent = globalThis.MediaPlaybackEvent ?? "
           "(globalThis.MediaPlaybackEvent = Object.freeze({\n"
        << "    PLAYBACK_STOPPED: 0,\n"
        << "    PLAYBACK_PLAYING: 1,\n"
        << "    PLAYBACK_PAUSED: 2,\n"
        << "    PLAYBACK_OTHER: 3\n"
        << "  }));\n"
        << "  const __normalizeTimerHandle = (timer) => {\n"
        << "    if (typeof timer === 'function') return timer.__timerId ?? 0;\n"
        << "    return timer ?? 0;\n"
        << "  };\n"
        << "  const __makeTimerHandle = (timerId) => {\n"
        << "    const stop = () => __native.clearTimer(timerId);\n"
        << "    stop.__timerId = timerId;\n"
        << "    return stop;\n"
        << "  };\n"
        << "  engine.setTimeout = (callback, delay = 0) => {\n"
        << "    return __makeTimerHandle(__native.setTimer(__instanceId, callback, Number(delay) "
           "|| 0, false));\n"
        << "  };\n"
        << "  engine.setInterval = (callback, delay = 0) => {\n"
        << "    return __makeTimerHandle(__native.setTimer(__instanceId, callback, Number(delay) "
           "|| 0, true));\n"
        << "  };\n"
        << "  engine.clearTimeout = (timer) => {\n"
        << "    if (timer !== undefined && timer !== null) "
           "__native.clearTimer(__normalizeTimerHandle(timer));\n"
        << "  };\n"
        << "  engine.clearInterval = engine.clearTimeout;\n"
        << "  if (typeof engine.isDesktopDevice !== 'function') engine.isDesktopDevice = () => "
           "true;\n"
        << "  if (typeof engine.isMobileDevice !== 'function') engine.isMobileDevice = () => "
           "false;\n"
        << "  if (typeof engine.isWallpaper !== 'function') engine.isWallpaper = () => true;\n"
        << "  if (typeof engine.isScreensaver !== 'function') engine.isScreensaver = () => false;\n"
        << "  if (typeof engine.isPortrait !== 'function') engine.isPortrait = () => "
           "engine.screenResolution.y > engine.screenResolution.x;\n"
        << "  if (typeof engine.isLandscape !== 'function') engine.isLandscape = () => "
           "engine.screenResolution.x >= engine.screenResolution.y;\n"
        << "  if (typeof engine.isRunningInEditor !== 'function') engine.isRunningInEditor = () => "
           "false;\n"
        << "  if (typeof engine.openUserShortcut !== 'function') engine.openUserShortcut = () => "
           "false;\n"
        << "  if (typeof engine.registerAsset !== 'function') engine.registerAsset = (file) => ({ "
           "file: String(file ?? '') });\n"
        << "  if (typeof engine.registerAudioBuffers !== 'function') {\n"
        << "    engine.registerAudioBuffers = (resolution) => {\n"
        << "      const size = Math.max(0, Number(resolution) || 0);\n"
        << "      const build = () => Array.from({ length: size }, () => 0);\n"
        << "      return { left: build(), right: build(), average: build() };\n"
        << "    };\n"
        << "  }\n"
        << "  if (engine.AUDIO_RESOLUTION_16 === undefined) engine.AUDIO_RESOLUTION_16 = 16;\n"
        << "  if (engine.AUDIO_RESOLUTION_32 === undefined) engine.AUDIO_RESOLUTION_32 = 32;\n"
        << "  if (engine.AUDIO_RESOLUTION_64 === undefined) engine.AUDIO_RESOLUTION_64 = 64;\n"
        << "  const __toNumber = (value, fallback = 0) => {\n"
        << "    const num = Number(value);\n"
        << "    return Number.isFinite(num) ? num : fallback;\n"
        << "  };\n"
        << "  const __parseVectorString = (value, size) => {\n"
        << "    const matches = String(value ?? '').match(/[-+]?\\d*\\.?\\d+(?:[eE][-+]?\\d+)?/g) "
           "?? [];\n"
        << "    const out = [];\n"
        << "    for (let i = 0; i < size; i++) out.push(__toNumber(matches[i], 0));\n"
        << "    return out;\n"
        << "  };\n"
        << "  const __vecValues = (value, size) => {\n"
        << "    if (value === undefined || value === null) return Array.from({ length: size }, () "
           "=> 0);\n"
        << "    if (typeof value === 'number') return Array.from({ length: size }, () => "
           "__toNumber(value, 0));\n"
        << "    if (typeof value === 'string') return __parseVectorString(value, size);\n"
        << "    if (Array.isArray(value)) return Array.from({ length: size }, (_, i) => "
           "__toNumber(value[i], 0));\n"
        << "    return Array.from({ length: size }, (_, i) => __toNumber(value[['x', 'y', 'z', "
           "'w'][i]], 0));\n"
        << "  };\n"
        << "  const __binaryVec = (self, value, op, Size, keys) => {\n"
        << "    const lhs = keys.map((key) => __toNumber(self[key], 0));\n"
        << "    const rhs = typeof value === 'number'\n"
        << "      ? Array.from({ length: lhs.length }, () => __toNumber(value, 0))\n"
        << "      : __vecValues(value, lhs.length);\n"
        << "    return new Size(...lhs.map((entry, index) => op(entry, rhs[index])));\n"
        << "  };\n"
        << "  const __dot = (lhs, rhs) => lhs.reduce((sum, value, index) => sum + value * "
           "rhs[index], 0);\n"
        << "  const __mixScalar = (a, b, t) => __toNumber(a, 0) + (__toNumber(b, 0) - "
           "__toNumber(a, 0)) * __toNumber(t, 0);\n"
        << "  const Vec2 = (typeof globalThis.Vec2 === 'function')\n"
        << "    ? globalThis.Vec2\n"
        << "    : (globalThis.Vec2 = class Vec2 {\n"
        << "        constructor(a = 0, b = 0) {\n"
        << "          const values = arguments.length === 0 ? [0, 0]\n"
        << "            : arguments.length === 1 ? __vecValues(a, 2)\n"
        << "            : [__toNumber(a, 0), __toNumber(b, 0)];\n"
        << "          this.x = values[0];\n"
        << "          this.y = values[1];\n"
        << "        }\n"
        << "        equals(other) { const v = __vecValues(other, 2); return Math.abs(this.x - "
           "v[0]) < 1e-6 && Math.abs(this.y - v[1]) < 1e-6; }\n"
        << "        length() { return Math.hypot(this.x, this.y); }\n"
        << "        lengthSqr() { return this.x * this.x + this.y * this.y; }\n"
        << "        normalize() { const len = this.length(); return len === 0 ? new Vec2() : new "
           "Vec2(this.x / len, this.y / len); }\n"
        << "        copy() { return new Vec2(this.x, this.y); }\n"
        << "        add(value) { return __binaryVec(this, value, (a, b) => a + b, Vec2, ['x', "
           "'y']); }\n"
        << "        subtract(value) { return __binaryVec(this, value, (a, b) => a - b, Vec2, ['x', "
           "'y']); }\n"
        << "        multiply(value) { return __binaryVec(this, value, (a, b) => a * b, Vec2, ['x', "
           "'y']); }\n"
        << "        divide(value) { return __binaryVec(this, value, (a, b) => a / b, Vec2, ['x', "
           "'y']); }\n"
        << "        dot(value) { const rhs = __vecValues(value, 2); return __dot([this.x, this.y], "
           "rhs); }\n"
        << "        reflect(normal) { const n = new Vec2(normal).normalize(); return "
           "this.subtract(n.multiply(2 * this.dot(n))); }\n"
        << "        mix(other, amount) { const rhs = __vecValues(other, 2); return new "
           "Vec2(__mixScalar(this.x, rhs[0], amount), __mixScalar(this.y, rhs[1], amount)); }\n"
        << "        min(value) { return __binaryVec(this, value, (a, b) => Math.min(a, b), Vec2, "
           "['x', 'y']); }\n"
        << "        max(value) { return __binaryVec(this, value, (a, b) => Math.max(a, b), Vec2, "
           "['x', 'y']); }\n"
        << "        perpendicular() { return new Vec2(-this.y, this.x); }\n"
        << "        abs() { return new Vec2(Math.abs(this.x), Math.abs(this.y)); }\n"
        << "        sign() { return new Vec2(Math.sign(this.x), Math.sign(this.y)); }\n"
        << "        round() { return new Vec2(Math.round(this.x), Math.round(this.y)); }\n"
        << "        floor() { return new Vec2(Math.floor(this.x), Math.floor(this.y)); }\n"
        << "        ceil() { return new Vec2(Math.ceil(this.x), Math.ceil(this.y)); }\n"
        << "        toString() { return `${this.x} ${this.y}`; }\n"
        << "      });\n"
        << "  const Vec3 = (typeof globalThis.Vec3 === 'function')\n"
        << "    ? globalThis.Vec3\n"
        << "    : (globalThis.Vec3 = class Vec3 {\n"
        << "        constructor(a = 0, b = 0, c = 0) {\n"
        << "          const values = arguments.length === 0 ? [0, 0, 0]\n"
        << "            : arguments.length === 1 ? __vecValues(a, 3)\n"
        << "            : arguments.length === 2 ? [__toNumber(a, 0), __toNumber(b, 0), 0]\n"
        << "            : [__toNumber(a, 0), __toNumber(b, 0), __toNumber(c, 0)];\n"
        << "          this.x = values[0];\n"
        << "          this.y = values[1];\n"
        << "          this.z = values[2];\n"
        << "        }\n"
        << "        equals(other) { const v = __vecValues(other, 3); return Math.abs(this.x - "
           "v[0]) < 1e-6 && Math.abs(this.y - v[1]) < 1e-6 && Math.abs(this.z - v[2]) < 1e-6; }\n"
        << "        length() { return Math.hypot(this.x, this.y, this.z); }\n"
        << "        lengthSqr() { return this.x * this.x + this.y * this.y + this.z * this.z; }\n"
        << "        normalize() { const len = this.length(); return len === 0 ? new Vec3() : new "
           "Vec3(this.x / len, this.y / len, this.z / len); }\n"
        << "        copy() { return new Vec3(this.x, this.y, this.z); }\n"
        << "        add(value) { return __binaryVec(this, value, (a, b) => a + b, Vec3, ['x', 'y', "
           "'z']); }\n"
        << "        subtract(value) { return __binaryVec(this, value, (a, b) => a - b, Vec3, ['x', "
           "'y', 'z']); }\n"
        << "        multiply(value) { return __binaryVec(this, value, (a, b) => a * b, Vec3, ['x', "
           "'y', 'z']); }\n"
        << "        divide(value) { return __binaryVec(this, value, (a, b) => a / b, Vec3, ['x', "
           "'y', 'z']); }\n"
        << "        dot(value) { const rhs = __vecValues(value, 3); return __dot([this.x, this.y, "
           "this.z], rhs); }\n"
        << "        reflect(normal) { const n = new Vec3(normal).normalize(); return "
           "this.subtract(n.multiply(2 * this.dot(n))); }\n"
        << "        mix(other, amount) { const rhs = __vecValues(other, 3); return new "
           "Vec3(__mixScalar(this.x, rhs[0], amount), __mixScalar(this.y, rhs[1], amount), "
           "__mixScalar(this.z, rhs[2], amount)); }\n"
        << "        min(value) { return __binaryVec(this, value, (a, b) => Math.min(a, b), Vec3, "
           "['x', 'y', 'z']); }\n"
        << "        max(value) { return __binaryVec(this, value, (a, b) => Math.max(a, b), Vec3, "
           "['x', 'y', 'z']); }\n"
        << "        cross(value) { const rhs = __vecValues(value, 3); return new Vec3(this.y * "
           "rhs[2] - this.z * rhs[1], this.z * rhs[0] - this.x * rhs[2], this.x * rhs[1] - this.y "
           "* rhs[0]); }\n"
        << "        abs() { return new Vec3(Math.abs(this.x), Math.abs(this.y), Math.abs(this.z)); "
           "}\n"
        << "        sign() { return new Vec3(Math.sign(this.x), Math.sign(this.y), "
           "Math.sign(this.z)); }\n"
        << "        round() { return new Vec3(Math.round(this.x), Math.round(this.y), "
           "Math.round(this.z)); }\n"
        << "        floor() { return new Vec3(Math.floor(this.x), Math.floor(this.y), "
           "Math.floor(this.z)); }\n"
        << "        ceil() { return new Vec3(Math.ceil(this.x), Math.ceil(this.y), "
           "Math.ceil(this.z)); }\n"
        << "        toString() { return `${this.x} ${this.y} ${this.z}`; }\n"
        << "      });\n"
        << "  // The native host normally seeds these cursor vectors before any script runs, but "
           "some\n"
        << "  // authored pointer-follow scripts read input.cursorWorldPosition.x every frame "
           "without a\n"
        << "  // guard. Upgrade plain native {x,y,z} cursor objects to Vec instances as well, "
           "because\n"
        << "  // Wallpaper Engine scripts commonly call vector helpers such as add() and subtract() "
           "on\n"
        << "  // input cursor positions during drag handling.\n"
        << "  if (!input.cursorWorldPosition || typeof input.cursorWorldPosition !== 'object' || "
           "typeof input.cursorWorldPosition.add !== 'function') input.cursorWorldPosition = new "
           "Vec3(input.cursorWorldPosition);\n"
        << "  if (!input.cursorScreenPosition || typeof input.cursorScreenPosition !== 'object' || "
           "typeof input.cursorScreenPosition.add !== 'function') input.cursorScreenPosition = "
           "new Vec2(input.cursorScreenPosition);\n"
        << "  // canvasSize/screenResolution remain plain mutable native objects in engineBase. The "
           "accessors above wrap each read as Vec2 so scripts get vector methods while resize updates "
           "remain visible without assigning through getter-only properties.\n"
        << "  const WEMath = (globalThis.WEMath && typeof globalThis.WEMath === 'object')\n"
        << "    ? globalThis.WEMath\n"
        << "    : (globalThis.WEMath = {\n"
        << "        deg2rad: Math.PI / 180,\n"
        << "        rad2deg: 180 / Math.PI,\n"
        << "        mix(a, b, t) {\n"
        << "          const lerp = (x, y, alpha) => Number(x) + (Number(y) - Number(x)) * "
           "Number(alpha);\n"
        << "          if (Array.isArray(a) && Array.isArray(b)) return a.map((value, index) => "
           "lerp(value, b[index] ?? value, t));\n"
        << "          if (a && b && typeof a === 'object' && typeof b === 'object') {\n"
        << "            const out = Array.isArray(a) ? [] : {};\n"
        << "            for (const key of Object.keys(a)) out[key] = lerp(a[key], b[key] ?? "
           "a[key], t);\n"
        << "            return out;\n"
        << "          }\n"
        << "          return lerp(a, b, t);\n"
        << "        },\n"
        << "        smoothStep(minValue, maxValue, value) {\n"
        << "          const edge0 = __toNumber(minValue, 0);\n"
        << "          const edge1 = __toNumber(maxValue, 1);\n"
        << "          if (edge0 === edge1) return value < edge0 ? 0 : 1;\n"
        << "          const x = Math.min(Math.max((__toNumber(value, 0) - edge0) / (edge1 - "
           "edge0), 0), 1);\n"
        << "          return x * x * (3 - 2 * x);\n"
        << "        },\n"
        << "        clamp(value, minValue, maxValue) {\n"
        << "          return Math.min(Math.max(Number(value), Number(minValue)), "
           "Number(maxValue));\n"
        << "        }\n"
        << "      });\n"
        << "  const WEColor = (globalThis.WEColor && typeof globalThis.WEColor === 'object')\n"
        << "    ? globalThis.WEColor\n"
        << "    : (globalThis.WEColor = {\n"
        << "        rgb2hsv(rgb) {\n"
        << "          const [r, g, b] = __vecValues(rgb, 3);\n"
        << "          const max = Math.max(r, g, b);\n"
        << "          const min = Math.min(r, g, b);\n"
        << "          const delta = max - min;\n"
        << "          let h = 0;\n"
        << "          if (delta !== 0) {\n"
        << "            if (max === r) h = ((g - b) / delta) % 6;\n"
        << "            else if (max === g) h = (b - r) / delta + 2;\n"
        << "            else h = (r - g) / delta + 4;\n"
        << "            h /= 6;\n"
        << "            if (h < 0) h += 1;\n"
        << "          }\n"
        << "          const s = max === 0 ? 0 : delta / max;\n"
        << "          return new Vec3(h, s, max);\n"
        << "        },\n"
        << "        hsv2rgb(hsv) {\n"
        << "          let [h, s, v] = __vecValues(hsv, 3);\n"
        << "          h = ((h % 1) + 1) % 1;\n"
        << "          const i = Math.floor(h * 6);\n"
        << "          const f = h * 6 - i;\n"
        << "          const p = v * (1 - s);\n"
        << "          const q = v * (1 - f * s);\n"
        << "          const t = v * (1 - (1 - f) * s);\n"
        << "          switch (i % 6) {\n"
        << "            case 0: return new Vec3(v, t, p);\n"
        << "            case 1: return new Vec3(q, v, p);\n"
        << "            case 2: return new Vec3(p, v, t);\n"
        << "            case 3: return new Vec3(p, q, v);\n"
        << "            case 4: return new Vec3(t, p, v);\n"
        << "            default: return new Vec3(v, p, q);\n"
        << "          }\n"
        << "        },\n"
        << "        normalizeColor(rgb) { const [r, g, b] = __vecValues(rgb, 3); return new Vec3(r "
           "/ 255, g / 255, b / 255); },\n"
        << "        expandColor(rgb) { const [r, g, b] = __vecValues(rgb, 3); return new Vec3(r * "
           "255, g * 255, b * 255); }\n"
        << "      });\n"
        << "  const WEVector = (globalThis.WEVector && typeof globalThis.WEVector === 'object')\n"
        << "    ? globalThis.WEVector\n"
        << "    : (globalThis.WEVector = {\n"
        << "        angleVector2(angle) {\n"
        << "          const radians = __toNumber(angle, 0) * WEMath.deg2rad;\n"
        << "          return new Vec2(Math.cos(radians), Math.sin(radians));\n"
        << "        },\n"
        << "        vectorAngle2(direction) {\n"
        << "          const [x, y] = __vecValues(direction, 2);\n"
        << "          return Math.atan2(y, x) * WEMath.rad2deg;\n"
        << "        }\n"
        << "      });\n"
        << "  const Mat4 = (typeof globalThis.Mat4 === 'function')\n"
        << "    ? globalThis.Mat4\n"
        << "    : (globalThis.Mat4 = class Mat4 extends Array {\n"
        << "        constructor() {\n"
        << "          super(16);\n"
        << "          for (let i = 0; i < 16; i++) this[i] = (i % 5 === 0) ? 1 : 0;\n"
        << "        }\n"
        << "        translation(position) {\n"
        << "          if (position === undefined) {\n"
        << "            return { x: Number(this[3] ?? 0), y: Number(this[7] ?? 0), z: "
           "Number(this[11] ?? 0) };\n"
        << "          }\n"
        << "          const source = Array.isArray(position)\n"
        << "            ? position\n"
        << "            : [position?.x ?? 0, position?.y ?? 0, position?.z ?? 0];\n"
        << "          this[3] = Number(source[0] ?? 0);\n"
        << "          this[7] = Number(source[1] ?? 0);\n"
        << "          this[11] = Number(source[2] ?? 0);\n"
        << "        }\n"
        << "        static fromArray(values) {\n"
        << "          const matrix = new Mat4();\n"
        << "          if (!Array.isArray(values)) return matrix;\n"
        << "          for (let i = 0; i < 16; i++) {\n"
        << "            const fallback = (i % 5 === 0) ? 1 : 0;\n"
        << "            matrix[i] = Number(values[i] ?? fallback);\n"
        << "          }\n"
        << "          return matrix;\n"
        << "        }\n"
        << "      });\n"
        << "  const __toMat4 = (value) => Array.isArray(value) ? Mat4.fromArray(value) : value;\n"
        << "  const localStorage = {\n"
        << "    LOCATION_GLOBAL: 'global',\n"
        << "    LOCATION_SCREEN: 'screen',\n"
        << "    get(key, location = 'screen') { return __native.localStorageGet(String(key), "
           "String(location)); },\n"
        << "    set(key, value, location = 'screen') { __native.localStorageSet(String(key), "
           "value, String(location)); },\n"
        << "    clear(location = 'screen') { __native.localStorageClear(String(location)); },\n"
        << "    delete(key, location = 'screen') { return "
           "!!__native.localStorageDelete(String(key), String(location)); },\n"
        << "    // Wallpaper Engine scene scripts use remove() as a single-key delete alias.\n"
        << "    remove(key, location = 'screen') { return this.delete(key, location); }\n"
        << "  };\n"
        << "  globalThis.localStorage = localStorage;\n"
        << "  function createScriptProperties() {\n"
        << "    const applyOption = function(opts) {\n"
        << "      if (opts && opts.name !== undefined && "
           "!Object.prototype.hasOwnProperty.call(__props, opts.name)) {\n"
        << "        let value = opts.value;\n"
        << "        if (value === undefined && Array.isArray(opts.options) && opts.options.length "
           "> 0) {\n"
        << "          const first = opts.options[0] ?? {};\n"
        << "          value = first.value !== undefined ? first.value : first.label;\n"
        << "        }\n"
        << "        __props[opts.name] = value;\n"
        << "      }\n"
        << "      return builder;\n"
        << "    };\n"
        << "    const target = { finish() { return __props; } };\n"
        << "    const builder = new Proxy(target, {\n"
        << "      get(obj, prop, receiver) {\n"
        << "        if (prop in obj) return Reflect.get(obj, prop, receiver);\n"
        << "        if (typeof prop === 'string' && prop.startsWith('add')) return applyOption;\n"
        << "        return undefined;\n"
        << "      }\n"
        << "    });\n"
        << "    return builder;\n"
        << "  }\n"
        << "  function createDeferredTextureAnimation() {\n"
        << "    // Deferred runtime layer placeholders intentionally have no mesh/material yet, "
           "but\n"
        << "    // authored wallpaper scripts may still drive their texture animation every "
           "frame.\n"
        << "    // Keep that script contract stable with a cheap no-op object so hidden "
           "multilingual\n"
        << "    // branches stay logical-only until visibility materialization is actually "
           "required.\n"
        << "    return {\n"
        << "      get frameCount() { return 0; },\n"
        << "      get duration() { return 0; },\n"
        << "      get rate() { return 1; },\n"
        << "      set rate(_value) {},\n"
        << "      play() {},\n"
        << "      stop() {},\n"
        << "      pause() {},\n"
        << "      isPlaying() { return false; },\n"
        << "      getFrame() { return 0; },\n"
        << "      setFrame(_frame) {},\n"
        << "      join() {}\n"
        << "    };\n"
        << "  }\n"
        << "  function createTextureAnimation(slot = 0, nodeId = __nodeId) {\n"
        << "    if (!__native.hasTextureAnimation(nodeId, slot)) {\n"
        << "      if (__native.isDeferredRuntimeLayer(nodeId)) return "
           "createDeferredTextureAnimation();\n"
        << "      return undefined;\n"
        << "    }\n"
        << "    return {\n"
        << "      get frameCount() { return __native.textureAnimationGet(nodeId, slot, "
           "'frameCount'); },\n"
        << "      get duration() { return __native.textureAnimationGet(nodeId, slot, 'duration'); "
           "},\n"
        << "      get rate() { return __native.textureAnimationGet(nodeId, slot, 'rate'); },\n"
        << "      set rate(value) { __native.textureAnimationSet(nodeId, slot, 'rate', value); },\n"
        << "      play() { __native.textureAnimationCall(nodeId, slot, 'play'); },\n"
        << "      stop() { __native.textureAnimationCall(nodeId, slot, 'stop'); },\n"
        << "      pause() { __native.textureAnimationCall(nodeId, slot, 'pause'); },\n"
        << "      isPlaying() { return __native.textureAnimationCall(nodeId, slot, 'isPlaying'); "
           "},\n"
        << "      getFrame() { return __native.textureAnimationCall(nodeId, slot, 'getFrame'); },\n"
        << "      setFrame(frame) { __native.textureAnimationCall(nodeId, slot, 'setFrame', "
           "frame); },\n"
        << "      join() { __native.textureAnimationCall(nodeId, slot, 'join'); }\n"
        << "    };\n"
        << "  }\n"
        << "  function createVideoTexture(nodeId = __nodeId) {\n"
        << "    // Authored WE scenes call getVideoTexture().play()/pause() on ordinary layer "
           "proxies.\n"
        << "    // Return a stable command object even when a layer has no video texture so those "
           "scripts\n"
        << "    // degrade to cheap no-ops instead of throwing on every render tick.\n"
        << "    return {\n"
        << "      play() { __native.videoTextureCall(nodeId, 'play'); },\n"
        << "      pause() { __native.videoTextureCall(nodeId, 'pause'); },\n"
        << "      stop() { __native.videoTextureCall(nodeId, 'stop'); },\n"
        << "      setCurrentTime(value) { __native.videoTextureCall(nodeId, 'setCurrentTime', "
           "value); },\n"
        << "      getCurrentTime() { return __native.videoTextureCall(nodeId, 'getCurrentTime'); "
           "},\n"
        << "      get rate() { return __native.videoTextureCall(nodeId, 'rate'); },\n"
        << "      set rate(value) { __native.videoTextureCall(nodeId, 'rate', value); },\n"
        << "      isPlaying() { return !!__native.videoTextureCall(nodeId, 'isPlaying'); }\n"
        << "    };\n"
        << "  }\n"
        << "  function createTimelineAnimation(animationId) {\n"
        << "    if (!(animationId > 0)) return undefined;\n"
        << "    return {\n"
        << "      get fps() { return __native.getPropertyAnimationProperty(animationId, 'fps'); "
           "},\n"
        << "      get frameCount() { return __native.getPropertyAnimationProperty(animationId, "
           "'frameCount'); },\n"
        << "      get duration() { return __native.getPropertyAnimationProperty(animationId, "
           "'duration'); },\n"
        << "      get name() { return __native.getPropertyAnimationProperty(animationId, 'name'); "
           "},\n"
        << "      get rate() { return __native.getPropertyAnimationProperty(animationId, 'rate'); "
           "},\n"
        << "      set rate(value) { __native.setPropertyAnimationProperty(animationId, 'rate', "
           "value); },\n"
        << "      play() { __native.propertyAnimationCall(animationId, 'play'); },\n"
        << "      stop() { __native.propertyAnimationCall(animationId, 'stop'); },\n"
        << "      pause() { __native.propertyAnimationCall(animationId, 'pause'); },\n"
        << "      isPlaying() { return __native.propertyAnimationCall(animationId, 'isPlaying'); "
           "},\n"
        << "      getFrame() { return __native.propertyAnimationCall(animationId, 'getFrame'); },\n"
        << "      setFrame(frame) { __native.propertyAnimationCall(animationId, 'setFrame', "
           "frame); }\n"
        << "    };\n"
        << "  }\n"
        << "  function createAnimationLayer(layerIndex, nodeId = __nodeId, instanceId = 0) {\n"
        << "    if (!__native.hasAnimationLayer(nodeId, layerIndex)) return undefined;\n"
        << "    return new Proxy({}, {\n"
        << "      get(_target, prop) {\n"
        << "        if (typeof prop !== 'string') return undefined;\n"
        << "        if (prop === 'getAnimation') return (name) => {\n"
        << "          const animationId = instanceId > 0\n"
        << "            ? __native.resolvePropertyAnimation(instanceId, name)\n"
        << "            : 0;\n"
        << "          return animationId > 0 ? createTimelineAnimation(animationId) : undefined;\n"
        << "        };\n"
        << "        if (prop === 'play') return () => __native.animationLayerCall(nodeId, "
           "layerIndex, 'play');\n"
        << "        if (prop === 'stop') return () => __native.animationLayerCall(nodeId, "
           "layerIndex, 'stop');\n"
        << "        if (prop === 'pause') return () => __native.animationLayerCall(nodeId, "
           "layerIndex, 'pause');\n"
        << "        if (prop === 'isPlaying') return () => __native.animationLayerCall(nodeId, "
           "layerIndex, 'isPlaying');\n"
        << "        if (prop === 'getFrame') return () => __native.animationLayerCall(nodeId, "
           "layerIndex, 'getFrame');\n"
        << "        if (prop === 'setFrame') return (frame) => __native.animationLayerCall(nodeId, "
           "layerIndex, 'setFrame', frame);\n"
        << "        if (prop === 'addEndedCallback') return (callback) => "
           "__native.addAnimationLayerEndedCallback(nodeId, layerIndex, callback);\n"
        << "        return __native.getAnimationLayerProperty(nodeId, layerIndex, prop);\n"
        << "      },\n"
        << "      set(_target, prop, value) {\n"
        << "        if (typeof prop !== 'string') return false;\n"
        << "        return !!__native.setAnimationLayerProperty(nodeId, layerIndex, prop, value);\n"
        << "      },\n"
        << "      has(_target, prop) {\n"
        << "        if (typeof prop !== 'string') return false;\n"
        << "        if (prop === 'getAnimation') return true;\n"
        << "        return !!__native.hasAnimationLayerMember(nodeId, layerIndex, prop);\n"
        << "      }\n"
        << "    });\n"
        << "  }\n"
        << "  // Effect material proxies route authored WE material properties through native "
           "alias resolution, so script names like raythreshold can update GLSL uniforms such "
           "as g_Threshold on the matching post-process pass.\n"
        << "  function createEffectMaterialProxy(nodeId, effectIndex, materialIndex = 0) {\n"
        << "    if (!__native.hasEffectMaterial(nodeId, effectIndex, materialIndex)) return "
           "undefined;\n"
        << "    return new Proxy({}, {\n"
        << "      get(_target, prop) {\n"
        << "        if (typeof prop !== 'string') return undefined;\n"
        << "        return __native.getEffectMaterialProperty(nodeId, effectIndex, "
           "materialIndex, prop);\n"
        << "      },\n"
        << "      set(_target, prop, value) {\n"
        << "        if (typeof prop !== 'string') return false;\n"
        << "        return !!__native.setEffectMaterialProperty(nodeId, effectIndex, "
           "materialIndex, prop, value);\n"
        << "      },\n"
        << "      has(_target, prop) {\n"
        << "        if (typeof prop !== 'string') return false;\n"
        << "        return !!__native.hasEffectMaterialMember(nodeId, effectIndex, "
           "materialIndex, prop);\n"
        << "      }\n"
        << "    });\n"
        << "  }\n"
        << "  function createEffectProxy(nodeId, effectIndex, instanceId = 0) {\n"
        << "    if (!__native.hasEffect(nodeId, effectIndex)) return undefined;\n"
        << "    return new Proxy({}, {\n"
        << "      get(_target, prop) {\n"
        << "        if (typeof prop !== 'string') return undefined;\n"
        << "        if (prop === 'getAnimation') return (name) => {\n"
        << "          const animationId = instanceId > 0 ? "
           "__native.resolvePropertyAnimation(instanceId, name) : 0;\n"
        << "          return animationId > 0 ? createTimelineAnimation(animationId) : undefined;\n"
        << "        };\n"
        << "        if (prop === 'getMaterial') return (materialIndex = 0) => "
           "createEffectMaterialProxy(nodeId, effectIndex, materialIndex);\n"
        << "        return __native.getEffectProperty(nodeId, effectIndex, prop);\n"
        << "      },\n"
        << "      set(_target, prop, value) {\n"
        << "        if (typeof prop !== 'string') return false;\n"
        << "        return !!__native.setEffectProperty(nodeId, effectIndex, prop, value);\n"
        << "      },\n"
        << "      has(_target, prop) {\n"
        << "        if (typeof prop !== 'string') return false;\n"
        << "        if (prop === 'getAnimation' || prop === 'getMaterial') return true;\n"
        << "        return !!__native.hasEffectMember(nodeId, effectIndex, prop);\n"
        << "      }\n"
        << "    });\n"
        << "  }\n"
        << "  function createLayerProxy(nodeId, instanceId = 0) {\n"
        << "    return new Proxy({}, {\n"
        << "      get(_target, prop) {\n"
        << "        if (typeof prop !== 'string') return undefined;\n"
        << "        if (prop === '__nodeId') return nodeId;\n"
        << "        if (prop === 'getTextureAnimation') return (slot = 0) => "
           "createTextureAnimation(slot, nodeId);\n"
        << "        if (prop === 'getVideoTexture') return () => createVideoTexture(nodeId);\n"
        << "        if (prop === 'getAnimation') return (name) => {\n"
        << "          const animationId = instanceId > 0\n"
        << "            ? __native.resolvePropertyAnimation(instanceId, name)\n"
        << "            : __native.resolveLayerAnimation(nodeId, name);\n"
        << "          return animationId > 0 ? createTimelineAnimation(animationId) : undefined;\n"
        << "        };\n"
        << "        if (prop === 'getAnimationLayerCount') return () => "
           "__native.getAnimationLayerCount(nodeId);\n"
        << "        if (prop === 'getAnimationLayer') return (nameOrIndex = 0) => {\n"
        << "          const index = __native.resolveAnimationLayer(nodeId, nameOrIndex);\n"
        << "          return index >= 0 ? createAnimationLayer(index, nodeId, instanceId) : "
           "undefined;\n"
        << "        };\n"
        << "        if (prop === 'getParent') return () => {\n"
        << "          const parentId = __native.getLayerRelation(nodeId, 'parent');\n"
        << "          // Some WE scripts call getParent().getTransformMatrix() even on imported "
           "scenes\n"
        << "          // whose logical parent metadata is absent. Returning the layer itself "
           "preserves a\n"
        << "          // stable transform object and avoids a per-frame TypeError while native "
           "parent\n"
        << "          // bindings continue to work when they are present.\n"
        << "          return parentId > 0 ? createLayerProxy(parentId) : "
           "createLayerProxy(nodeId);\n"
        << "        };\n"
        << "        if (prop === 'getChildren') return () => {\n"
        << "          const ids = __native.getLayerChildren(nodeId);\n"
        << "          return Array.isArray(ids) ? ids.map((id) => createLayerProxy(id)) : [];\n"
        << "        };\n"
        << "        if (prop === 'setParent') return (parent, attachmentOrAdjust, "
           "adjustTransforms) => __native.layerCall(nodeId, 'setParent', parent, "
           "attachmentOrAdjust, adjustTransforms);\n"
        << "        if (prop === 'getAttachmentIndex') return (name) => __native.layerCall(nodeId, "
           "'getAttachmentIndex', name);\n"
        << "        if (prop === 'getAttachmentMatrix') return (attachment) => "
           "__toMat4(__native.layerCall(nodeId, 'getAttachmentMatrix', attachment));\n"
        << "        if (prop === 'getTransformMatrix') return () => __native.layerCall(nodeId, "
           "'getTransformMatrix');\n"
        << "        if (prop === 'getAttachmentOrigin') return (attachment) => "
           "__native.layerCall(nodeId, 'getAttachmentOrigin', attachment);\n"
        << "        if (prop === 'getAttachmentAngles') return (attachment) => "
           "__native.layerCall(nodeId, 'getAttachmentAngles', attachment);\n"
        << "        if (prop === 'getBoneCount') return () => __native.layerCall(nodeId, "
           "'getBoneCount');\n"
        << "        if (prop === 'getBoneIndex') return (name) => __native.layerCall(nodeId, "
           "'getBoneIndex', name);\n"
        << "        if (prop === 'getBoneParentIndex') return (bone) => __native.layerCall(nodeId, "
           "'getBoneParentIndex', bone);\n"
        << "        if (prop === 'getBoneTransform') return (bone) => "
           "__toMat4(__native.layerCall(nodeId, 'getBoneTransform', bone));\n"
        << "        if (prop === 'getLocalBoneTransform') return (bone) => "
           "__toMat4(__native.layerCall(nodeId, 'getLocalBoneTransform', bone));\n"
        << "        if (prop === 'getLocalBoneAngles') return (bone) => __native.layerCall(nodeId, "
           "'getLocalBoneAngles', bone);\n"
        << "        if (prop === 'getLocalBoneOrigin') return (bone) => __native.layerCall(nodeId, "
           "'getLocalBoneOrigin', bone);\n"
        << "        if (prop === 'setBoneTransform') return (bone, transform) => "
           "__native.layerCall(nodeId, 'setBoneTransform', bone, transform);\n"
        << "        if (prop === 'setLocalBoneTransform') return (bone, transform) => "
           "__native.layerCall(nodeId, 'setLocalBoneTransform', bone, transform);\n"
        << "        if (prop === 'setLocalBoneAngles') return (bone, angles) => "
           "__native.layerCall(nodeId, 'setLocalBoneAngles', bone, angles);\n"
        << "        if (prop === 'setLocalBoneOrigin') return (bone, origin) => "
           "__native.layerCall(nodeId, 'setLocalBoneOrigin', bone, origin);\n"
        << "        if (prop === 'applyBonePhysicsImpulse') return (bone, impulse) => "
           "__native.layerCall(nodeId, 'applyBonePhysicsImpulse', bone, impulse);\n"
        << "        if (prop === 'rotateObjectSpace') return (angles) => "
           "__native.rotateLayerObjectSpace(nodeId, angles);\n"
        << "        if (prop === 'play') return __native.hasLayerMember(nodeId, 'play') ? () => "
           "__native.layerCall(nodeId, 'play') : undefined;\n"
        << "        if (prop === 'stop') return __native.hasLayerMember(nodeId, 'stop') ? () => "
           "__native.layerCall(nodeId, 'stop') : undefined;\n"
        << "        if (prop === 'pause') return __native.hasLayerMember(nodeId, 'pause') ? () => "
           "__native.layerCall(nodeId, 'pause') : undefined;\n"
        << "        if (prop === 'isPlaying') return __native.hasLayerMember(nodeId, 'isPlaying') "
           "? () => __native.layerCall(nodeId, 'isPlaying') : undefined;\n"
        << "        return __native.getLayerPropertyById(nodeId, prop);\n"
        << "      },\n"
        << "      set(_target, prop, value) {\n"
        << "        if (typeof prop !== 'string') return false;\n"
        << "        return !!__native.setLayerPropertyById(nodeId, prop, value);\n"
        << "      },\n"
        << "      has(_target, prop) {\n"
        << "        if (typeof prop !== 'string') return false;\n"
        << "        return !!__native.hasLayerMember(nodeId, prop);\n"
        << "      }\n"
        << "    });\n"
        << "  }\n"
        << "  function createThisObjectProxy() {\n"
        << "    const animationObject = __objectKind === 'animationLayer'\n"
        << "      ? (createAnimationLayer(__objectIndex, __nodeId, __instanceId) ?? undefined)\n"
        << "      : undefined;\n"
        << "    const effectObject = __objectKind === 'effect'\n"
        << "      ? (createEffectProxy(__nodeId, __objectIndex, __instanceId) ?? undefined)\n"
        << "      : undefined;\n"
        << "    const baseObject = effectObject ?? animationObject ?? createLayerProxy(__nodeId, "
           "__instanceId);\n"
        << "    return new Proxy({}, {\n"
        << "      get(_target, prop) {\n"
        << "        if (typeof prop !== 'string') return undefined;\n"
        << "        if (prop === 'getAnimation') {\n"
        << "          const baseMember = baseObject ? baseObject[prop] : undefined;\n"
        << "          if (typeof baseMember === 'function') return baseMember.bind(baseObject);\n"
        << "          return (name) => {\n"
        << "            const animationId = __instanceId > 0\n"
        << "              ? __native.resolvePropertyAnimation(__instanceId, name)\n"
        << "              : __native.resolveLayerAnimation(__nodeId, name);\n"
        << "            return animationId > 0 ? createTimelineAnimation(animationId) : "
           "undefined;\n"
        << "          };\n"
        << "        }\n"
        << "        if (prop === 'addEndedCallback' && __objectKind === 'animationLayer') {\n"
        << "          const baseMember = animationObject ? animationObject[prop] : undefined;\n"
        << "          if (typeof baseMember === 'function') return "
           "baseMember.bind(animationObject);\n"
        << "          return (callback) => __native.addAnimationLayerEndedCallback(__nodeId, "
           "__objectIndex, callback);\n"
        << "        }\n"
        << "        const value = baseObject ? baseObject[prop] : undefined;\n"
        << "        return typeof value === 'function' ? value.bind(baseObject) : value;\n"
        << "      },\n"
        << "      set(_target, prop, value) {\n"
        << "        if (baseObject == null || typeof prop !== 'string') return false;\n"
        << "        baseObject[prop] = value;\n"
        << "        return true;\n"
        << "      },\n"
        << "      has(_target, prop) {\n"
        << "        if (typeof prop !== 'string') return false;\n"
        << "        if (prop === 'getAnimation') return true;\n"
        << "        if (prop === 'addEndedCallback' && __objectKind === 'animationLayer') return "
           "true;\n"
        << "        return baseObject != null ? (prop in baseObject) : false;\n"
        << "      }\n"
        << "    });\n"
        << "  }\n"
        << "  function createLayerAssetHandle(file) {\n"
        << "    const handle = { file };\n"
        << "    // Imported workshop scene scripts keep their namespace in __workshopId while "
           "their\n"
        << "    // runtime createLayer() calls often use compact asset paths. Carry that "
           "namespace to\n"
        << "    // native code so the VFS-backed resolver can choose between the project-local "
           "asset and\n"
        << "    // the imported workshop asset without embedding resource-type knowledge in the "
           "JS shim.\n"
        << "    if (typeof __workshopId === 'string' && __workshopId.length > 0) {\n"
        << "      handle.workshopId = __workshopId;\n"
        << "    }\n"
        << "    return handle;\n"
        << "  }\n"
        << "  function normalizeCreateLayerConfig(value, topLevel = false) {\n"
        << "    if (value === null || value === undefined) return value;\n"
        << "    if (topLevel && typeof value === 'string') return "
           "createLayerAssetHandle(value);\n"
        << "    if (typeof value !== 'object') return value;\n"
        << "    if ('__nodeId' in value) return value.__nodeId;\n"
        << "    if (Array.isArray(value)) return value.map((item) => "
           "normalizeCreateLayerConfig(item, false));\n"
        << "    const keys = Object.keys(value);\n"
        << "    if (keys.length === 1 && keys[0] === 'file' && typeof value.file === 'string') {\n"
        << "      return topLevel ? createLayerAssetHandle(value.file) : value.file;\n"
        << "    }\n"
        << "    const normalized = {};\n"
        << "    for (const [key, entry] of Object.entries(value)) {\n"
        << "      normalized[key] = normalizeCreateLayerConfig(entry, false);\n"
        << "    }\n"
        << "    return normalized;\n"
        << "  }\n"
        << "  const __layerProxy = createLayerProxy(__nodeId, 0);\n"
        << "  const __sceneUpdateCallbacks = [];\n"
        << "  const __sceneMethods = {\n"
        << "    on(eventName, callback) {\n"
        << "      // Wallpaper Engine scene.on('update', fn) handlers are dispatched from the "
           "same\n"
        << "      // per-frame bridge as exported update() functions so global scene scripts can "
           "drive\n"
        << "      // layer visibility without registering a separate native callback type.\n"
        << "      if (String(eventName).toLowerCase() === 'update' && typeof callback === "
           "'function') {\n"
        << "        __sceneUpdateCallbacks.push(callback);\n"
        << "        return callback;\n"
        << "      }\n"
        << "      return undefined;\n"
        << "    },\n"
        << "    getLayerCount() { return __native.getSceneLayerCount(); },\n"
        << "    enumerateLayers() {\n"
        << "      const ids = __native.enumerateSceneLayers();\n"
        << "      return Array.isArray(ids) ? ids.map((id) => createLayerProxy(id)) : [];\n"
        << "    },\n"
        << "    getLayer(nameOrIndex) {\n"
        << "      const id = __native.getSceneLayer(nameOrIndex);\n"
        << "      return id > 0 ? createLayerProxy(id) : undefined;\n"
        << "    },\n"
        << "    destroyLayer(layer) {\n"
        << "      return !!__native.destroySceneLayer(layer);\n"
        << "    },\n"
        << "    createLayer(configuration) {\n"
        << "      const layerId = "
           "__native.createSceneLayer(normalizeCreateLayerConfig(configuration, true));\n"
        << "      return layerId > 0 ? createLayerProxy(layerId) : undefined;\n"
        << "    },\n"
        << "    sortLayer(layer, index) {\n"
        << "      return !!__native.sortSceneLayer(layer, index);\n"
        << "    },\n"
        << "    getLayerIndex(layer) {\n"
        << "      return __native.getSceneLayerIndex(layer);\n"
        << "    },\n"
        << "    getInitialLayerConfig(layer) {\n"
        << "      return __native.getInitialSceneLayerConfig(layer);\n"
        << "    }\n"
        << "  };\n"
        << "  const __sceneProxy = new Proxy(__sceneMethods, {\n"
        << "    get(target, prop, receiver) {\n"
        << "      if (typeof prop === 'string' && __native.hasSceneMember(prop)) {\n"
        << "        return __native.getSceneProperty(prop);\n"
        << "      }\n"
        << "      return Reflect.get(target, prop, receiver);\n"
        << "    },\n"
        << "    set(target, prop, value, receiver) {\n"
        << "      if (typeof prop === 'string' && __native.hasSceneMember(prop)) {\n"
        << "        return !!__native.setSceneProperty(prop, value);\n"
        << "      }\n"
        << "      return Reflect.set(target, prop, value, receiver);\n"
        << "    },\n"
        << "    has(target, prop) {\n"
        << "      return (typeof prop === 'string' && __native.hasSceneMember(prop)) || "
           "Reflect.has(target, prop);\n"
        << "    }\n"
        << "  });\n"
        << "  // Wallpaper Engine does not make thisLayer immutable; a few authored scene scripts "
           "rebind\n"
        << "  // it to another layer inside their module scope. Keep a separate __layerProxy for "
           "native\n"
        << "  // bookkeeping, but expose thisLayer as a let binding so those scripts instantiate "
           "cleanly.\n"
        << "  let thisLayer = __layerProxy;\n"
        << "  const thisObject = createThisObjectProxy();\n"
        << "  const thisScene = __sceneProxy;\n"
        << "  const scene = __sceneProxy;\n"
        << body << "\n"
        << "  const __exportedUpdate = typeof update === 'function' ? update : undefined;\n"
        << "  const __dispatchUpdate = (...args) => {\n"
        << "    // Property update hooks may be ticked by scene events without an explicit value. "
           "Reuse\n"
        << "    // the current native property so scripts that mutate value.x/value.y still "
           "receive the\n"
        << "    // mutable vector object Wallpaper Engine would normally pass.\n"
        << "    const updateArgs = args.length > 0 ? args : ((typeof __propertyName === 'string' "
           "&& __propertyName.length > 0)\n"
        << "      ? [__layerProxy[__propertyName]]\n"
        << "      : args);\n"
        << "    let result = undefined;\n"
        << "    for (const callback of __sceneUpdateCallbacks) {\n"
        << "      const callbackResult = callback(...updateArgs);\n"
        << "      if (callbackResult !== undefined) result = callbackResult;\n"
        << "    }\n"
        << "    if (__exportedUpdate) {\n"
        << "      const updateResult = __exportedUpdate(...updateArgs);\n"
        << "      if (updateResult !== undefined) result = updateResult;\n"
        << "    }\n"
        << "    return result;\n"
        << "  };\n"
        << "  return {\n"
        << "    init: typeof init === 'function' ? init : undefined,\n"
        << "    update: (__exportedUpdate || __sceneUpdateCallbacks.length > 0) ? __dispatchUpdate "
           ": undefined,\n"
        << "    applyUserProperties: typeof applyUserProperties === 'function' ? "
           "applyUserProperties : undefined,\n"
        << "    applyGeneralSettings: typeof applyGeneralSettings === 'function' ? "
           "applyGeneralSettings : undefined,\n"
        << "    cursorEnter: typeof cursorEnter === 'function' ? cursorEnter : undefined,\n"
        << "    cursorLeave: typeof cursorLeave === 'function' ? cursorLeave : undefined,\n"
        << "    cursorMove: typeof cursorMove === 'function' ? cursorMove : undefined,\n"
        << "    cursorDown: typeof cursorDown === 'function' ? cursorDown : undefined,\n"
        << "    cursorUp: typeof cursorUp === 'function' ? cursorUp : undefined,\n"
        << "    cursorClick: typeof cursorClick === 'function' ? cursorClick : undefined,\n"
        << "    mediaThumbnailChanged: typeof mediaThumbnailChanged === 'function' ? "
           "mediaThumbnailChanged : undefined,\n"
        << "    mediaPropertiesChanged: typeof mediaPropertiesChanged === 'function' ? "
           "mediaPropertiesChanged : undefined,\n"
        << "    // The native media dispatcher looks up this exact export name when playback "
           "changes.\n"
        << "    // Keeping it in the returned table lets MediaPlaybackEvent users run inside the "
           "same\n"
        << "    // closure where the compatibility enum above is defined.\n"
        << "    mediaPlaybackChanged: typeof mediaPlaybackChanged === 'function' ? "
           "mediaPlaybackChanged : undefined,\n"
        << "    destroy: typeof destroy === 'function' ? destroy : undefined,\n"
        << "    resizeScreen: typeof resizeScreen === 'function' ? resizeScreen : undefined,\n"
        << "    __debugInspect: () => JSON.stringify({\n"
        << "      objectKind: __objectKind,\n"
        << "      hasAddEndedCallback: ('addEndedCallback' in thisObject),\n"
        << "      addEndedCallbackType: typeof thisObject.addEndedCallback,\n"
        << "      getAnimationType: typeof thisObject.getAnimation,\n"
        << "      playType: typeof thisObject.play,\n"
        << "      setFrameType: typeof thisObject.setFrame,\n"
        << "      frameCountType: typeof thisObject.frameCount,\n"
        << "      sharedOffsetedStartAniType: typeof shared.offsetedStartAni\n"
        << "    })\n"
        << "  };\n"
        << "})";
    return wrapper.str();
}

void LogQuickJSException(JSContext* context, const char* stage) {
    JSValue exception = JS_GetException(context);

    std::string message = "<unknown>";
    if (const char* text = JS_ToCString(context, exception)) {
        message = text;
        JS_FreeCString(context, text);
    }

    std::string stack;
    JSValue     stack_value = JS_GetPropertyStr(context, exception, "stack");
    if (! JS_IsException(stack_value) && ! JS_IsUndefined(stack_value)) {
        if (const char* text = JS_ToCString(context, stack_value)) {
            stack = text;
            JS_FreeCString(context, text);
        }
    }
    JS_FreeValue(context, stack_value);

    if (stack.empty()) {
        LOG_ERROR("QuickJS %s failed: %s", stage, message.c_str());
    } else {
        LOG_ERROR("QuickJS %s failed: %s\n%s", stage, message.c_str(), stack.c_str());
    }

    JS_FreeValue(context, exception);
}

bool ReadJSNumber(JSContext* context, JSValueConst value, double* out_value) {
    if (out_value == nullptr) return false;
    if (JS_IsException(value) || JS_IsUndefined(value) || JS_IsNull(value)) return false;
    if (JS_ToFloat64(context, out_value, value) != 0) return false;
    return std::isfinite(*out_value);
}

bool ReadJSString(JSContext* context, JSValueConst value, std::string* out_value) {
    if (out_value == nullptr) return false;
    const char* text = JS_ToCString(context, value);
    if (text == nullptr) return false;
    *out_value = text;
    JS_FreeCString(context, text);
    return true;
}

JSValue NumericVectorToJS(JSContext* context, const std::vector<double>& values) {
    static constexpr const char* names[] = { "x", "y", "z", "w" };

    if (values.size() <= 1) {
        return JS_NewFloat64(context, values.empty() ? 0.0 : values.front());
    }

    if (values.size() == 2 || values.size() == 3) {
        JSValue global = JS_GetGlobalObject(context);
        JSValue ctor   = JS_GetPropertyStr(context, global, values.size() == 2 ? "Vec2" : "Vec3");
        if (! JS_IsException(ctor) && JS_IsFunction(context, ctor)) {
            std::array<JSValue, 3> args {
                JS_NewFloat64(context, values[0]),
                JS_NewFloat64(context, values[1]),
                JS_NewFloat64(context, values.size() > 2 ? values[2] : 0.0),
            };
            JSValue result =
                JS_CallConstructor(context, ctor, static_cast<int>(values.size()), args.data());
            for (size_t index = 0; index < values.size(); index++) {
                JS_FreeValue(context, args[index]);
            }
            JS_FreeValue(context, ctor);
            JS_FreeValue(context, global);
            if (! JS_IsException(result)) {
                return result;
            }
            JS_FreeValue(context, result);
            JS_FreeValue(context, JS_GetException(context));
        } else {
            JS_FreeValue(context, ctor);
            JS_FreeValue(context, global);
        }
    }

    if (values.size() <= 4) {
        JSValue object = JS_NewObject(context);
        for (size_t index = 0; index < values.size(); index++) {
            JS_SetPropertyStr(context, object, names[index], JS_NewFloat64(context, values[index]));
        }
        return object;
    }

    JSValue array = JS_NewArray(context);
    for (uint32_t index = 0; index < values.size(); index++) {
        JS_SetPropertyUint32(
            context, array, index, JS_NewFloat64(context, values[static_cast<size_t>(index)]));
    }
    return array;
}

JSValue Matrix4ToJS(JSContext* context, const Eigen::Matrix4d& matrix) {
    JSValue  array = JS_NewArray(context);
    uint32_t index = 0;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            JS_SetPropertyUint32(context, array, index++, JS_NewFloat64(context, matrix(row, col)));
        }
    }
    return array;
}

JSValue TransformMatrixToJS(JSContext* context, const Eigen::Matrix4d& matrix) {
    JSValue  array = JS_NewArray(context);
    uint32_t index = 0;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            JS_SetPropertyUint32(context, array, index++, JS_NewFloat64(context, matrix(row, col)));
        }
    }

    // Wallpaper Engine script examples commonly read transform.m[12]/m[13] for layer origin.
    // Those indices are column-major translation slots, so expose the array through .m as WE
    // does while leaving bone/attachment matrix helpers untouched.
    JS_SetPropertyStr(context, array, "m", JS_DupValue(context, array));
    return array;
}

JSValue Vec3ToJS(JSContext* context, const std::array<double, 3>& value) {
    return NumericVectorToJS(context, { value[0], value[1], value[2] });
}

JSValue ParseJsonToJS(JSContext* context, const std::string& json) {
    return JS_ParseJSON(context, json.c_str(), json.size(), "<scene-layer-config>");
}

std::optional<nlohmann::json> JsonFromJS(JSContext* context, JSValueConst value) {
    JSValue global    = JS_GetGlobalObject(context);
    JSValue json      = JS_GetPropertyStr(context, global, "JSON");
    JSValue stringify = JS_GetPropertyStr(context, json, "stringify");

    JSValue result = JS_UNDEFINED;
    if (! JS_IsException(stringify) && JS_IsFunction(context, stringify)) {
        JSValue argument = JS_DupValue(context, value);
        result           = JS_Call(context, stringify, json, 1, &argument);
        JS_FreeValue(context, argument);
    }

    JS_FreeValue(context, stringify);
    JS_FreeValue(context, json);
    JS_FreeValue(context, global);

    if (JS_IsException(result) || JS_IsUndefined(result)) {
        JS_FreeValue(context, result);
        return std::nullopt;
    }

    std::string json_text;
    const bool  ok = ReadJSString(context, result, &json_text);
    JS_FreeValue(context, result);
    if (! ok) return std::nullopt;

    try {
        return nlohmann::json::parse(json_text);
    } catch (const std::exception& e) {
        LOG_ERROR("failed to parse createLayer configuration JSON: %s", e.what());
        return std::nullopt;
    }
}

bool IsAssetHandleJson(const nlohmann::json& json) {
    if (! json.is_object() || ! json.contains("file") || ! json.at("file").is_string()) {
        return false;
    }

    // Script-side createLayer() may attach the stripped module's __workshopId to a file handle.
    // Keep the accepted shape intentionally tiny so arbitrary layer configuration objects cannot
    // be mistaken for asset handles just because they happen to contain a "file" property.
    for (const auto& [key, value] : json.items()) {
        (void)value;
        if (key != "file" && key != "workshopId") return false;
    }
    return true;
}

struct ScriptAssetHandle {
    std::string file;
    std::string workshop_id;
};

struct AssetPathParts {
    std::string_view root;
    std::string_view relative_path;
};

constexpr std::string_view kAssetMountPrefix { "/assets/" };
constexpr std::string_view kWorkshopNamespaceSegment { "workshop" };

std::optional<ScriptAssetHandle> ReadScriptAssetHandle(const nlohmann::json& json) {
    if (json.is_string()) {
        return ScriptAssetHandle {
            .file = json.get<std::string>(),
        };
    }

    if (! IsAssetHandleJson(json)) return std::nullopt;

    ScriptAssetHandle handle {
        .file = json.at("file").get<std::string>(),
    };
    if (const auto workshop_it = json.find("workshopId");
        workshop_it != json.end() && workshop_it->is_string()) {
        handle.workshop_id = workshop_it->get<std::string>();
    }
    return handle;
}

std::string AssetVfsPath(std::string_view file) {
    std::string path;
    path.reserve(kAssetMountPrefix.size() + file.size());
    path.append(kAssetMountPrefix);
    path.append(file);
    return path;
}

std::optional<AssetPathParts> SplitAssetPath(std::string_view file) {
    const auto separator = file.find('/');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= file.size()) {
        return std::nullopt;
    }

    return AssetPathParts {
        .root          = file.substr(0, separator),
        .relative_path = file.substr(separator + 1),
    };
}

bool IsWorkshopScopedAssetPath(const AssetPathParts& parts) {
    return parts.relative_path == kWorkshopNamespaceSegment ||
           (parts.relative_path.starts_with(kWorkshopNamespaceSegment) &&
            parts.relative_path.size() > kWorkshopNamespaceSegment.size() &&
            parts.relative_path[kWorkshopNamespaceSegment.size()] == '/');
}

std::optional<std::string> BuildWorkshopScopedAssetFile(const ScriptAssetHandle& handle) {
    const auto parts = SplitAssetPath(handle.file);
    if (! parts.has_value() || IsWorkshopScopedAssetPath(*parts) || handle.workshop_id.empty()) {
        return std::nullopt;
    }

    // Imported Wallpaper Engine scripts commonly preserve only the first asset directory and the
    // leaf path. Rebuilding `<root>/workshop/<id>/<relative>` from path structure keeps the rule
    // open to new asset roots while VFS validation remains the authority for whether it is real.
    std::string resolved;
    resolved.reserve(parts->root.size() + 1 + kWorkshopNamespaceSegment.size() + 1 +
                     handle.workshop_id.size() + 1 + parts->relative_path.size());
    resolved.append(parts->root);
    resolved.push_back('/');
    resolved.append(kWorkshopNamespaceSegment);
    resolved.push_back('/');
    resolved.append(handle.workshop_id);
    resolved.push_back('/');
    resolved.append(parts->relative_path);
    return resolved;
}

std::string ResolveWorkshopRelativeAssetFile(const Scene* scene, const ScriptAssetHandle& handle) {
    if (handle.file.empty() || scene == nullptr || scene->vfs == nullptr) {
        return handle.file;
    }

    if (scene->vfs->Contains(AssetVfsPath(handle.file))) return handle.file;

    const auto scoped_file = BuildWorkshopScopedAssetFile(handle);
    if (! scoped_file.has_value() || ! scene->vfs->Contains(AssetVfsPath(*scoped_file))) {
        LOG_INFO("SceneScriptCreateLayer: no workshop asset fallback found file='%s' "
                 "workshop-id='%s'",
                 handle.file.c_str(),
                 handle.workshop_id.c_str());
        return handle.file;
    }

    LOG_INFO("SceneScriptCreateLayer: resolved workshop asset file='%s' workshop-id='%s' "
             "resolved='%s'",
             handle.file.c_str(),
             handle.workshop_id.c_str(),
             scoped_file->c_str());
    return *scoped_file;
}

bool IsWorkshopFallbackEligible(const ScriptAssetHandle& handle) {
    const std::string_view file { handle.file };
    // Keep path-shape screening separate from VFS lookup. The resolver should only rewrite
    // ordinary relative asset paths; malformed or absolute strings stay untouched so the existing
    // materializer reports the same unsupported-asset diagnostics it would have reported before.
    const bool malformed_asset_path = file.starts_with('/') ||
                                      file.find('\\') != std::string_view::npos ||
                                      file.find("//") != std::string_view::npos ||
                                      file.find("..") != std::string_view::npos;
    return ! handle.workshop_id.empty() && SplitAssetPath(file).has_value() &&
           ! malformed_asset_path;
}

std::string ResolveScriptAssetFile(const Scene* scene, const ScriptAssetHandle& handle) {
    // Asset resolution is intentionally isolated from dynamic layer materialization: this helper
    // only decides which registered asset path exists, while the caller remains responsible for
    // inferring whether that asset is an image, particle, sound, or unsupported dynamic layer.
    return IsWorkshopFallbackEligible(handle) ? ResolveWorkshopRelativeAssetFile(scene, handle)
                                              : handle.file;
}

nlohmann::json NormalizeCreateLayerJson(const nlohmann::json& json) {
    if (json.is_array()) {
        nlohmann::json array = nlohmann::json::array();
        for (const auto& item : json) {
            array.push_back(NormalizeCreateLayerJson(item));
        }
        return array;
    }

    if (! json.is_object()) return json;
    if (IsAssetHandleJson(json)) return json.at("file").get<std::string>();

    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, value] : json.items()) {
        object[key] = NormalizeCreateLayerJson(value);
    }
    return object;
}

std::optional<nlohmann::json> MaterializeAssetHandleConfig(const Scene*          scene,
                                                           const nlohmann::json& json) {
    const auto asset_handle = ReadScriptAssetHandle(json);
    if (! asset_handle.has_value()) return std::nullopt;

    std::string file = ResolveScriptAssetFile(scene, *asset_handle);
    if (file.empty()) return std::nullopt;

    if (file.ends_with(".png") || file.ends_with(".jpg") || file.ends_with(".jpeg") ||
        file.ends_with(".webp")) {
        LOG_ERROR("dynamic image layers require the registered image model JSON, not a raw texture "
                  "file: %s",
                  file.c_str());
        return std::nullopt;
    }

    nlohmann::json config = nlohmann::json::object();
    if (file.ends_with(".ttf") || file.ends_with(".otf")) {
        LOG_ERROR("dynamic text layers backed by font assets are not supported yet: %s",
                  file.c_str());
        return std::nullopt;
    }
    if (file.ends_with(".mp3") || file.ends_with(".wav") || file.ends_with(".ogg")) {
        config["name"]    = file;
        config["sound"]   = nlohmann::json::array({ file });
        config["visible"] = true;
        return config;
    }

    bool inferred = false;
    if (file.starts_with("models/")) {
        config["image"] = file;
        inferred        = true;
    } else if (file.starts_with("particles/")) {
        config["particle"] = file;
        inferred           = true;
    } else if (file.ends_with(".json") && scene != nullptr && scene->vfs != nullptr) {
        nlohmann::json asset_json;
        if (PARSE_JSON(fs::GetFileContent(*scene->vfs, "/assets/" + file), asset_json)) {
            if (asset_json.contains("material")) {
                config["image"] = file;
                inferred        = true;
            } else if (asset_json.contains("emitter")) {
                config["particle"] = file;
                inferred           = true;
            }
        }
    }

    if (! inferred) {
        LOG_ERROR("unable to infer dynamic layer type from registered asset: %s", file.c_str());
        return std::nullopt;
    }

    if (! config.contains("name")) {
        const auto slash = file.find_last_of('/');
        const auto start = slash == std::string::npos ? 0 : slash + 1;
        config["name"]   = file.substr(start);
    }
    return config;
}

JSValue ScriptValueToJS(JSContext* context, const WPScriptValue& value) {
    switch (value.shape) {
    case WPScriptValueShape::Number:
        return JS_NewFloat64(context,
                             value.numeric_values.empty() ? 0.0 : value.numeric_values.front());
    case WPScriptValueShape::Boolean: return JS_NewBool(context, value.boolean_value);
    case WPScriptValueShape::String:
        return JS_NewStringLen(context, value.string_value.c_str(), value.string_value.size());
    case WPScriptValueShape::NumberArray:
    case WPScriptValueShape::VectorString: return NumericVectorToJS(context, value.numeric_values);
    }

    return JS_UNDEFINED;
}

std::optional<WPScriptValue> ScriptValueFromJS(JSContext* context, JSValueConst value,
                                               WPDynamicValue::Type hint) {
    if (hint == WPDynamicValue::Type::String) {
        std::string text;
        if (! ReadJSString(context, value, &text)) return std::nullopt;
        return WPScriptValue::String(std::move(text));
    }

    if (hint == WPDynamicValue::Type::Boolean) {
        const int result = JS_ToBool(context, value);
        if (result < 0) return std::nullopt;
        return WPScriptValue::Boolean(result != 0);
    }

    const auto expected_size = [&]() -> size_t {
        switch (hint) {
        case WPDynamicValue::Type::Float2: return 2;
        case WPDynamicValue::Type::Float3:
        case WPDynamicValue::Type::Int3: return 3;
        case WPDynamicValue::Type::Float4: return 4;
        default: return 1;
        }
    }();

    std::vector<double> values;
    values.reserve(expected_size);

    if (expected_size == 1) {
        double number = 0.0;
        if (! ReadJSNumber(context, value, &number)) return std::nullopt;
        values.push_back(number);
    } else if (JS_IsObject(value) && ! JS_IsArray(value)) {
        static constexpr const char* names[] = { "x", "y", "z", "w" };
        for (size_t i = 0; i < expected_size; i++) {
            JSValue    property = JS_GetPropertyStr(context, value, names[i]);
            double     number   = 0.0;
            const bool ok = ! JS_IsException(property) && ReadJSNumber(context, property, &number);
            JS_FreeValue(context, property);
            if (! ok) return std::nullopt;
            values.push_back(number);
        }
    } else if (JS_IsArray(value)) {
        for (uint32_t i = 0; i < expected_size; i++) {
            JSValue    item = JS_GetPropertyUint32(context, value, i);
            double     number { 0.0 };
            const bool ok = ! JS_IsException(item) && ReadJSNumber(context, item, &number);
            JS_FreeValue(context, item);
            if (! ok) return std::nullopt;
            values.push_back(number);
        }
    } else {
        double scalar = 0.0;
        if (! ReadJSNumber(context, value, &scalar)) return std::nullopt;
        values.assign(expected_size, scalar);
    }

    if (expected_size <= 1) return WPScriptValue::Number(values.front());
    return WPScriptValue::NumberArray(std::move(values));
}

std::optional<WPScriptValue> UserPropertyToScriptValue(const UserPropertyValue& property) {
    if (auto dynamic = WPDynamicValue::FromUserPropertyValue(property, WPDynamicValue::Type::Null);
        dynamic.has_value()) {
        return dynamic->toScriptValue();
    }
    return std::nullopt;
}

double ComputeTimeOfDay() {
    using clock         = std::chrono::system_clock;
    const auto now_time = clock::to_time_t(clock::now());
    const auto local    = std::localtime(&now_time);
    if (local == nullptr) return 0.0;

    const auto seconds = ((local->tm_hour * 60) + local->tm_min) * 60 + local->tm_sec;
    return static_cast<double>(seconds) / (24.0 * 60.0 * 60.0);
}

} // namespace

struct WPSceneScriptHost::Opaque {
    Scene*                                       scene { nullptr };
    RuntimeState                                 runtime;
    JSValue                                      shared { JS_UNDEFINED };
    JSValue                                      engine_base { JS_UNDEFINED };
    JSValue                                      console { JS_UNDEFINED };
    JSValue                                      input { JS_UNDEFINED };
    JSValue                                      general_settings_object { JS_UNDEFINED };
    JSValue                                      scene_object { JS_UNDEFINED };
    JSValue                                      native_bridge { JS_UNDEFINED };
    JSValue                                      user_properties_object { JS_UNDEFINED };
    uint32_t                                     next_instance_id { 1 };
    uint32_t                                     next_property_animation_id { 1 };
    uint64_t                                     next_timer_id { 1 };
    double                                       runtime_seconds { 0.0 };
    bool                                         initialized { false };
    bool                                         applying_user_properties { false };
    std::vector<WPSceneScriptRegistration>       property_bindings;
    std::vector<PropertyAnimationInstance>       property_animations;
    std::vector<std::unique_ptr<ScriptInstance>> instances;
    std::vector<ScriptTimer>                     timers;
    std::vector<AudioBufferBinding>              audio_buffers;
    std::vector<float>                           external_audio_samples;
    std::vector<int32_t>                         pending_destroy_layer_ids;
    UserPropertyMap                              user_properties;
    UserPropertyMap                              dispatched_user_properties;
    std::unordered_set<std::string>              user_property_names;
    std::unordered_map<std::string, std::string> general_settings;
    std::unordered_map<std::string, std::string> dispatched_general_settings;
    std::unordered_set<std::string>              general_setting_names;
    nlohmann::json                               local_storage_global { nlohmann::json::object() };
    nlohmann::json                               local_storage_screen { nlohmann::json::object() };
    std::unordered_map<SceneNode*, std::unordered_map<usize, TextureAnimationState>> texture_states;
    std::unordered_map<SceneNode*, std::unordered_map<usize, AnimationLayerRuntimeState>>
                                 animation_layer_states;
    WPSceneScriptMediaState      media_state;
    WPSceneScriptMediaState      dispatched_media_state;
    std::unordered_set<uint32_t> hovered_instances;
    std::unordered_set<uint32_t> pressed_instances;
    std::vector<SceneRegistrationRange> pending_scene_registration_ranges;
    std::vector<int32_t>                deferred_residency_warmup_layer_ids;
    std::size_t                         deferred_residency_warmup_index { 0 };
};

std::optional<WPDynamicValue> ReadOriginalLayerPropertyValue(
    const WPSceneScriptHost::Opaque* opaque, int32_t layer_id, std::string_view property_name) {
    if (property_name != "originalOrigin" || opaque == nullptr || opaque->scene == nullptr)
        return std::nullopt;

    const auto initial_it = opaque->scene->initialLayerConfigJson.find(layer_id);
    if (initial_it == opaque->scene->initialLayerConfigJson.end()) return std::nullopt;

    nlohmann::json layer_config;
    try {
        layer_config = nlohmann::json::parse(initial_it->second);
    } catch (const std::exception& e) {
        LOG_ERROR("failed to parse initial layer configuration JSON for layer %d: %s",
                  layer_id,
                  e.what());
        return std::nullopt;
    }

    if (! layer_config.is_object()) return std::nullopt;
    auto property_it = layer_config.find("origin");
    if (property_it == layer_config.end()) return std::nullopt;

    const nlohmann::json* value_node = &(*property_it);
    if (value_node->is_object()) {
        const auto nested_value = value_node->find("value");
        if (nested_value != value_node->end()) value_node = &(*nested_value);
    }

    // Wallpaper Engine exposes originalOrigin as the authored base layer origin, not the
    // script-updated runtime origin. Dynamic origin properties are stored as objects with a "value"
    // field, so unwrap that field before converting it to the script-facing Vec3.
    return WPDynamicValue::FromJsonLiteral(*value_node, WPDynamicValue::Type::Float3);
}

std::optional<WPDynamicValue> ReadParticlePropertyValue(const WPSceneScriptHost::Opaque* opaque,
                                                        int32_t layer_id,
                                                        std::string_view property_name) {
    if ((!IsParticleColorProperty(property_name) && !IsParticleSizeProperty(property_name) &&
         !IsParticleRateProperty(property_name)) ||
        opaque == nullptr || opaque->scene == nullptr || layer_id == 0) {
        return std::nullopt;
    }

    const auto particle_it = opaque->scene->objectRuntimeParticleSubsystems.find(layer_id);
    if (particle_it == opaque->scene->objectRuntimeParticleSubsystems.end()) return std::nullopt;

    for (const auto* subsystem : particle_it->second) {
        if (subsystem == nullptr) continue;
        if (IsParticleColorProperty(property_name)) {
            if (auto color = subsystem->RuntimeColorOverride(); color.has_value()) {
                return WPDynamicValue(*color);
            }
        }
        if (IsParticleSizeProperty(property_name)) {
            if (auto size = subsystem->RuntimeSizeOverride(); size.has_value()) {
                return WPDynamicValue(*size);
            }
        }
        if (IsParticleRateProperty(property_name)) {
            if (auto rate = subsystem->RuntimeRateOverride(); rate.has_value()) {
                return WPDynamicValue(*rate);
            }
        }
    }
    return std::nullopt;
}

bool ApplyParticlePropertyValue(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                                std::string_view property_name, const WPDynamicValue& value) {
    if ((!IsParticleColorProperty(property_name) && !IsParticleSizeProperty(property_name) &&
         !IsParticleRateProperty(property_name)) ||
        opaque == nullptr || opaque->scene == nullptr || layer_id == 0) {
        return false;
    }

    const auto particle_it = opaque->scene->objectRuntimeParticleSubsystems.find(layer_id);

    if (IsParticleColorProperty(property_name)) {
        const auto color = NormalizeParticleColorValue(property_name, value);
        if (!color.has_value()) {
            LOG_ERROR("SceneParticleColorApply: layer=%d property='%.*s' invalid-value=%s",
                      layer_id,
                      static_cast<int>(property_name.size()),
                      property_name.data(),
                      value.describe().c_str());
            return false;
        }

        if (particle_it == opaque->scene->objectRuntimeParticleSubsystems.end()) {
            if (opaque->scene->deferredRuntimeParticleLayerIds.count(layer_id) != 0) {
                // Hidden runtime-controlled particle layers do not allocate their ParticleSubSystem
                // until visibility turns true. Accept the color edit now; deferred materialization
                // will parse the same instanceoverride property against the latest user-property
                // snapshot.
                LOG_VERBOSE("SceneParticleColorDeferred: layer=%d property='%.*s' "
                            "color=[%.3f, %.3f, %.3f]",
                            layer_id,
                            static_cast<int>(property_name.size()),
                            property_name.data(),
                            (*color)[0],
                            (*color)[1],
                            (*color)[2]);
                return true;
            }
            return false;
        }

        std::size_t target_count = 0;
        for (auto* subsystem : particle_it->second) {
            if (subsystem == nullptr) continue;
            subsystem->SetRuntimeColorOverride(*color);
            target_count++;
        }

        if (target_count == 0) return false;

        LOG_VERBOSE("SceneParticleColorApply: layer=%d property='%.*s' color=[%.3f, %.3f, %.3f] "
                    "subsystem-targets=%zu",
                    layer_id,
                    static_cast<int>(property_name.size()),
                    property_name.data(),
                    (*color)[0],
                    (*color)[1],
                    (*color)[2],
                    target_count);
        return true;
    }

    if (IsParticleRateProperty(property_name)) {
        const auto rate = NormalizeParticleRateValue(value);
        if (!rate.has_value()) {
            LOG_ERROR("SceneParticleRateApply: layer=%d property='%.*s' invalid-value=%s",
                      layer_id,
                      static_cast<int>(property_name.size()),
                      property_name.data(),
                      value.describe().c_str());
            return false;
        }

        if (particle_it == opaque->scene->objectRuntimeParticleSubsystems.end()) {
            if (opaque->scene->deferredRuntimeParticleLayerIds.count(layer_id) != 0) {
                // A deferred particle layer has no live subsystem yet. Accept the script value so
                // hidden layers do not fail their update loop; once visibility materializes the
                // subsystem, the next script tick will write the same clock multiplier to it.
                LOG_VERBOSE("SceneParticleRateDeferred: layer=%d property='%.*s' rate=%.3f",
                            layer_id,
                            static_cast<int>(property_name.size()),
                            property_name.data(),
                            *rate);
                return true;
            }
            return false;
        }

        std::size_t target_count = 0;
        for (auto* subsystem : particle_it->second) {
            if (subsystem == nullptr) continue;
            subsystem->SetRuntimeRateOverride(*rate);
            target_count++;
        }

        if (target_count == 0) return false;
        // Audio-reactive particle rate scripts commonly return a value every frame. Keep the
        // successful hot path silent so real diagnostics such as invalid values or deferred-layer
        // materialization remain visible without flooding the log or perturbing frame pacing.
        return true;
    }

    if (particle_it == opaque->scene->objectRuntimeParticleSubsystems.end() &&
        opaque->scene->deferredRuntimeParticleLayerIds.count(layer_id) == 0) {
        return false;
    }

    const auto size = NormalizeParticleSizeValue(value);
    if (!size.has_value()) {
        LOG_ERROR("SceneParticleSizeApply: layer=%d property='%.*s' invalid-value=%s",
                  layer_id,
                  static_cast<int>(property_name.size()),
                  property_name.data(),
                  value.describe().c_str());
        return false;
    }

    if (particle_it == opaque->scene->objectRuntimeParticleSubsystems.end()) {
        if (opaque->scene->deferredRuntimeParticleLayerIds.count(layer_id) != 0) {
            // Size shares the same deferred contract as color: a hidden logical particle has no
            // ParticleSubSystem yet, so the next materialization pass must pick up the latest
            // user-property value from the stored scene JSON.
            LOG_INFO("SceneParticleSizeDeferred: layer=%d property='%.*s' size=%.3f",
                     layer_id,
                     static_cast<int>(property_name.size()),
                     property_name.data(),
                     *size);
            return true;
        }
        return false;
    }

    std::size_t target_count = 0;
    for (auto* subsystem : particle_it->second) {
        if (subsystem == nullptr) continue;
        subsystem->SetRuntimeSizeOverride(*size);
        target_count++;
    }

    if (target_count == 0) return false;

    LOG_INFO("SceneParticleSizeApply: layer=%d property='%.*s' size=%.3f subsystem-targets=%zu",
             layer_id,
             static_cast<int>(property_name.size()),
             property_name.data(),
             *size,
             target_count);
    return true;
}

bool GetExternalAudioBufferValues(WPSceneScriptHost::Opaque* opaque, uint32_t resolution,
                                  std::vector<float>* left, std::vector<float>* right,
                                  std::vector<float>* average) {
    if (opaque == nullptr || left == nullptr || right == nullptr || average == nullptr ||
        opaque->external_audio_samples.empty()) {
        return false;
    }

    const auto&        samples   = opaque->external_audio_samples;
    const size_t       half_size = samples.size() >= 2 ? samples.size() / 2 : 0;
    std::vector<float> left_source;
    std::vector<float> right_source;
    if (half_size > 0 && samples.size() % 2 == 0) {
        left_source.assign(samples.begin(), samples.begin() + static_cast<ptrdiff_t>(half_size));
        right_source.assign(samples.begin() + static_cast<ptrdiff_t>(half_size), samples.end());
    } else {
        left_source  = samples;
        right_source = samples;
    }

    const auto left_source_raw  = left_source;
    const auto right_source_raw = right_source;
    left_source                 = NormalizeExternalSpectrumChannel(left_source);
    right_source                = NormalizeExternalSpectrumChannel(right_source);

    *left  = ResampleSpectrumChannel(left_source, resolution);
    *right = ResampleSpectrumChannel(right_source, resolution);
    average->assign(static_cast<size_t>(resolution), 0.0f);
    for (size_t i = 0; i < average->size(); i++) {
        const float left_value  = i < left->size() ? (*left)[i] : 0.0f;
        const float right_value = i < right->size() ? (*right)[i] : 0.0f;
        (*average)[i]           = (left_value + right_value) * 0.5f;
    }

    return true;
}

std::string LocalStoragePath(const WPSceneScriptHost::Opaque* opaque) {
    const auto scene_id =
        opaque != nullptr && opaque->scene != nullptr && ! opaque->scene->scene_id.empty()
            ? opaque->scene->scene_id
            : std::string("unknown_id");
    return "/cache/scenescript-localstorage/" + scene_id + ".json";
}

void LoadLocalStorage(WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr || opaque->scene == nullptr || opaque->scene->vfs == nullptr) return;

    auto&      vfs  = *opaque->scene->vfs;
    const auto path = LocalStoragePath(opaque);
    if (! vfs.Contains(path)) return;

    nlohmann::json root;
    if (! PARSE_JSON(fs::GetFileContent(vfs, path), root)) return;
    if (root.contains("global") && root.at("global").is_object()) {
        opaque->local_storage_global = root.at("global");
    }
    if (root.contains("screen") && root.at("screen").is_object()) {
        opaque->local_storage_screen = root.at("screen");
    }
}

void SaveLocalStorage(const WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr || opaque->scene == nullptr || opaque->scene->vfs == nullptr) return;

    auto stream = opaque->scene->vfs->OpenW(LocalStoragePath(opaque));
    if (! stream) return;

    nlohmann::json root = {
        { "global", opaque->local_storage_global },
        { "screen", opaque->local_storage_screen },
    };
    const std::string payload = root.dump();
    stream->Write(payload.data(), payload.size());
}

bool EnsureLocalStorageBucketObject(nlohmann::json* bucket, std::string_view location_name) {
    if (bucket == nullptr) return false;
    if (bucket->is_object()) return true;

    // Corrupted or legacy localStorage content is recoverable because WE scripts expect the
    // bucket to be recreated on demand. Log it as informational cleanup instead of an engine
    // error so expected self-healing does not pollute run.log.
    LOG_INFO("scene localStorage bucket '%.*s' is not an object, resetting",
             static_cast<int>(location_name.size()),
             location_name.data());
    *bucket = nlohmann::json::object();
    return true;
}

namespace
{

TextLayerRuntimeState*       FindTextLayerById(WPSceneScriptHost::Opaque* opaque, int32_t layer_id);
const TextLayerRuntimeState* FindTextLayerById(const WPSceneScriptHost::Opaque* opaque,
                                               int32_t                          layer_id);

WPSceneScriptHost::Opaque* GetOpaque(JSContext* context) {
    return static_cast<WPSceneScriptHost::Opaque*>(JS_GetContextOpaque(context));
}

ScriptInstance* FindInstance(WPSceneScriptHost::Opaque* opaque, uint32_t instance_id) {
    for (const auto& instance : opaque->instances) {
        if (instance->instance_id == instance_id) return instance.get();
    }
    return nullptr;
}

std::string DescribeScriptInstance(const WPSceneScriptHost::Opaque* opaque, uint32_t instance_id) {
    std::ostringstream description;
    description << "instance=" << instance_id;
    if (opaque == nullptr) return description.str();

    const auto* instance =
        FindInstance(const_cast<WPSceneScriptHost::Opaque*>(opaque), instance_id);
    if (instance == nullptr) return description.str();

    const auto& registration = instance->registration;
    description << ", target=" << TargetKindName(registration.target_kind)
                << ", objectId=" << registration.object_id;
    if (! registration.property_name.empty()) {
        description << ", property=" << registration.property_name;
    }
    if (registration.target_kind == WPSceneScriptTargetKind::AnimationLayer ||
        registration.target_kind == WPSceneScriptTargetKind::Effect) {
        description << ", targetIndex=" << registration.target_index;
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Effect) {
        description << ", targetId=" << registration.target_id;
    }

    if (auto* node =
            FindNodeById(const_cast<WPSceneScriptHost::Opaque*>(opaque), registration.object_id);
        node != nullptr && ! node->Name().empty()) {
        description << ", layer=" << node->Name();
    }

    return description.str();
}

SceneNode* FindInstanceNode(WPSceneScriptHost::Opaque* opaque, uint32_t instance_id) {
    auto* instance = FindInstance(opaque, instance_id);
    return instance != nullptr ? instance->registration.node : nullptr;
}

PropertyAnimationInstance* FindPropertyAnimation(WPSceneScriptHost::Opaque* opaque,
                                                 uint32_t                   animation_id) {
    if (opaque == nullptr) return nullptr;
    for (auto& animation : opaque->property_animations) {
        if (animation.animation_id == animation_id) return &animation;
    }
    return nullptr;
}

const PropertyAnimationInstance* FindPropertyAnimation(const WPSceneScriptHost::Opaque* opaque,
                                                       uint32_t animation_id) {
    if (opaque == nullptr) return nullptr;
    for (const auto& animation : opaque->property_animations) {
        if (animation.animation_id == animation_id) return &animation;
    }
    return nullptr;
}

PropertyAnimationInstance* FindPropertyAnimation(WPSceneScriptHost::Opaque* opaque, SceneNode* node,
                                                 std::string_view animation_name) {
    if (opaque == nullptr || node == nullptr) return nullptr;
    for (auto& animation : opaque->property_animations) {
        // Camera layers expose property animations through the same script-facing layer object as
        // empty/image layers. Include camera targets here so thisLayer.getAnimation("zoom") can
        // resolve the authored camera timeline instead of disappearing behind the target split.
        if (animation.registration.node != node ||
            (animation.registration.target_kind != WPSceneScriptTargetKind::Layer &&
             animation.registration.target_kind != WPSceneScriptTargetKind::Camera)) {
            continue;
        }

        // Wallpaper Engine accepts both the owning property name and the authored timeline name
        // here. The latter is required by click scripts such as getAnimation("LogoShake") where the
        // animated property is "alpha" but the editor-visible animation name is "LogoShake".
        const bool matches_property_name = animation.registration.property_name == animation_name;
        const bool matches_timeline_name = animation.registration.animation != nullptr &&
                                           animation.registration.animation->name == animation_name;
        if (matches_property_name || matches_timeline_name) {
            return &animation;
        }
    }
    return nullptr;
}

const PropertyAnimationInstance* FindPropertyAnimation(const WPSceneScriptHost::Opaque* opaque,
                                                       SceneNode*                       node,
                                                       std::string_view property_name) {
    if (opaque == nullptr || node == nullptr) return nullptr;
    for (const auto& animation : opaque->property_animations) {
        // Runtime property reads use the same node identity for layer and camera registrations;
        // keeping both target kinds discoverable prevents camera keyframes from being treated as
        // unrelated state when scripts query animation handles by property name.
        if (animation.registration.node == node &&
            animation.registration.property_name == property_name &&
            (animation.registration.target_kind == WPSceneScriptTargetKind::Layer ||
             animation.registration.target_kind == WPSceneScriptTargetKind::Camera)) {
            return &animation;
        }
    }
    return nullptr;
}

bool SameRegistrationTarget(const WPSceneScriptRegistration& lhs,
                            const WPSceneScriptRegistration& rhs) {
    if (lhs.target_kind != rhs.target_kind || lhs.object_id != rhs.object_id) return false;

    if (lhs.target_kind == WPSceneScriptTargetKind::Effect) {
        // Authored effect ids survive parse-time pruning better than array positions, but older
        // registrations may only have an index. Prefer ids when both sides have one and fall back
        // to the materialized index for generated/synthetic effect entries.
        if (lhs.target_id != 0 && rhs.target_id != 0) return lhs.target_id == rhs.target_id;
        return lhs.target_index == rhs.target_index;
    }

    if (lhs.target_kind == WPSceneScriptTargetKind::AnimationLayer) {
        return lhs.target_index == rhs.target_index;
    }

    return lhs.node == rhs.node || lhs.node == nullptr || rhs.node == nullptr;
}

PropertyAnimationInstance* FindPropertyAnimation(WPSceneScriptHost::Opaque*        opaque,
                                                 uint32_t                          instance_id,
                                                 const std::optional<std::string>& property_name) {
    if (opaque == nullptr) return nullptr;

    const auto* instance = FindInstance(opaque, instance_id);
    if (instance == nullptr) return nullptr;

    const auto resolved_name = property_name.has_value() && ! property_name->empty()
                                   ? *property_name
                                   : instance->registration.property_name;
    for (auto& animation : opaque->property_animations) {
        if (animation.registration.property_name == resolved_name &&
            SameRegistrationTarget(animation.registration, instance->registration)) {
            return &animation;
        }
    }
    return nullptr;
}

WPShaderValueUpdater* GetShaderUpdater(const WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr || opaque->scene == nullptr || ! opaque->scene->shaderValueUpdater)
        return nullptr;
    return dynamic_cast<WPShaderValueUpdater*>(opaque->scene->shaderValueUpdater.get());
}

const WPShaderValueData* GetNodeData(const WPSceneScriptHost::Opaque* opaque, SceneNode* node) {
    auto* updater = GetShaderUpdater(opaque);
    return updater != nullptr && node != nullptr ? updater->GetNodeData(node) : nullptr;
}

WPShaderValueData* GetNodeData(WPSceneScriptHost::Opaque* opaque, SceneNode* node) {
    auto* updater = GetShaderUpdater(opaque);
    return updater != nullptr && node != nullptr ? updater->GetNodeData(node) : nullptr;
}

SceneNode* FindNodeById(WPSceneScriptHost::Opaque* opaque, int32_t node_id) {
    if (opaque == nullptr || opaque->scene == nullptr) return nullptr;
    auto it = opaque->scene->layerNodes.find(node_id);
    return it == opaque->scene->layerNodes.end() ? nullptr : it->second;
}

void RebindLayerRegistrations(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                              SceneNode* node) {
    if (opaque == nullptr || opaque->scene == nullptr) return;

    auto rebind_registration = [layer_id, node](WPSceneScriptRegistration& registration) {
        if (registration.object_id == layer_id &&
            (registration.target_kind == WPSceneScriptTargetKind::Layer ||
             registration.target_kind == WPSceneScriptTargetKind::Effect)) {
            // Deferred text/particle materialization replaces the lightweight logical node with
            // the real runtime node. Effect registrations still target the effect by id/index, but
            // their script environment also needs the owner layer node id for thisObject helpers.
            registration.node = node;
        }
    };

    for (auto& registration : opaque->scene->bindingRegistrations) {
        rebind_registration(registration);
    }
    for (auto& registration : opaque->scene->scriptRegistrations) {
        rebind_registration(registration);
    }
    for (auto& registration : opaque->scene->propertyAnimationRegistrations) {
        rebind_registration(registration);
    }
    for (auto& registration : opaque->property_bindings) {
        rebind_registration(registration);
    }
    for (auto& animation : opaque->property_animations) {
        rebind_registration(animation.registration);
    }
    for (auto& instance : opaque->instances) {
        if (instance != nullptr) rebind_registration(instance->registration);
    }
}

SceneRegistrationRange CaptureSceneRegistrationRange(WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr || opaque->scene == nullptr) {
        return SceneRegistrationRange {};
    }

    return SceneRegistrationRange {
        .binding_start = opaque->scene->bindingRegistrations.size(),
        .animation_start = opaque->scene->propertyAnimationRegistrations.size(),
        .script_start = opaque->scene->scriptRegistrations.size(),
    };
}

bool SceneRegistrationRangeHasNewEntries(WPSceneScriptHost::Opaque* opaque,
                                         const SceneRegistrationRange& range) {
    if (opaque == nullptr || opaque->scene == nullptr) return false;
    return range.binding_start < range.binding_end ||
           range.animation_start < range.animation_end ||
           range.script_start < range.script_end;
}

void RegisterSceneRegistrationRange(WPSceneScriptHost::Opaque* opaque,
                                    const SceneRegistrationRange& range) {
    if (opaque == nullptr || opaque->scene == nullptr) return;

    auto resolved_range = range;
    if (resolved_range.binding_end == std::numeric_limits<std::size_t>::max()) {
        resolved_range.binding_end = opaque->scene->bindingRegistrations.size();
    }
    if (resolved_range.animation_end == std::numeric_limits<std::size_t>::max()) {
        resolved_range.animation_end = opaque->scene->propertyAnimationRegistrations.size();
    }
    if (resolved_range.script_end == std::numeric_limits<std::size_t>::max()) {
        resolved_range.script_end = opaque->scene->scriptRegistrations.size();
    }
    resolved_range.binding_end = std::max(
        resolved_range.binding_start,
        std::min(resolved_range.binding_end, opaque->scene->bindingRegistrations.size()));
    resolved_range.animation_end = std::max(
        resolved_range.animation_start,
        std::min(resolved_range.animation_end,
                 opaque->scene->propertyAnimationRegistrations.size()));
    resolved_range.script_end = std::max(
        resolved_range.script_start,
        std::min(resolved_range.script_end, opaque->scene->scriptRegistrations.size()));
    if (! SceneRegistrationRangeHasNewEntries(opaque, resolved_range)) return;

    if (opaque->applying_user_properties) {
        // A visible-property write can materialize a hidden language branch while ApplyUserProperties()
        // is iterating the current binding vector. Queue the newly parsed registrations and attach
        // them after that pass, so vector growth cannot invalidate the active dispatch entry while the
        // same user-property payload still reaches the late-created bindings.
        opaque->pending_scene_registration_ranges.push_back(std::move(resolved_range));
        return;
    }

    if (! opaque->scene->scriptHost) return;

    auto* host = opaque->scene->scriptHost.get();

    for (std::size_t i = resolved_range.binding_start; i < resolved_range.binding_end; ++i) {
        host->RegisterPropertyBinding(opaque->scene->bindingRegistrations[i]);
    }
    for (std::size_t i = resolved_range.animation_start; i < resolved_range.animation_end; ++i) {
        host->RegisterPropertyAnimation(opaque->scene->propertyAnimationRegistrations[i]);
    }
    for (std::size_t i = resolved_range.script_start; i < resolved_range.script_end; ++i) {
        host->RegisterPropertyScript(opaque->scene->scriptRegistrations[i]);
    }
}

void FlushPendingSceneRegistrationRanges(WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr) return;

    while (! opaque->pending_scene_registration_ranges.empty()) {
        std::vector<SceneRegistrationRange> pending;
        pending.swap(opaque->pending_scene_registration_ranges);
        for (const auto& range : pending) {
            RegisterSceneRegistrationRange(opaque, range);
        }
    }
}

bool MaterializeDeferredParticleLayerIfNeeded(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                                              bool mark_graph_dirty) {
    if (opaque == nullptr || opaque->scene == nullptr) return false;
    if (opaque->scene->deferredRuntimeParticleLayerIds.count(layer_id) == 0) return true;

    const auto registration_range = CaptureSceneRegistrationRange(opaque);

    if (! wallpaper::MaterializeDeferredParticleLayer(
            *opaque->scene, layer_id, &opaque->user_properties)) {
        LOG_ERROR("DeferredRuntimeParticleRealize: failed for layer=%d", layer_id);
        return false;
    }

    auto* node = FindNodeById(opaque, layer_id);
    if (node == nullptr) {
        LOG_ERROR("DeferredRuntimeParticleRealize: missing realized node for layer=%d", layer_id);
        return false;
    }

    RebindLayerRegistrations(opaque, layer_id, node);
    RegisterSceneRegistrationRange(opaque, registration_range);
    EnsureTextureAnimationStatesForNode(opaque, node);
    // Realizing a deferred particle layer inserts the actual runtime scene node and the particle
    // render work that did not exist when the graph was compiled with only the hidden placeholder.
    // A resource refresh cannot add that missing pass, so the first false->true visibility toggle
    // must force a topology rebuild before the layer can become visible on screen.
    if (mark_graph_dirty) opaque->scene->MarkRenderGraphTopologyDirty();
    LOG_VERBOSE("DeferredRuntimeParticleRealize: materialized layer=%d topology-dirty=%s",
                layer_id,
                mark_graph_dirty ? "true" : "false");
    return true;
}

bool MaterializeDeferredImageLayerIfNeeded(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                                           bool mark_graph_dirty) {
    if (opaque == nullptr || opaque->scene == nullptr) return false;
    if (opaque->scene->deferredRuntimeImageLayerIds.count(layer_id) == 0) return true;

    const auto registration_range = CaptureSceneRegistrationRange(opaque);

    if (! wallpaper::MaterializeDeferredImageLayer(
            *opaque->scene, layer_id, &opaque->user_properties)) {
        LOG_ERROR("DeferredRuntimeImageRealize: failed for layer=%d", layer_id);
        return false;
    }

    auto* node = FindNodeById(opaque, layer_id);
    if (node == nullptr) {
        LOG_ERROR("DeferredRuntimeImageRealize: missing realized node for layer=%d", layer_id);
        return false;
    }

    RebindLayerRegistrations(opaque, layer_id, node);
    RegisterSceneRegistrationRange(opaque, registration_range);
    EnsureTextureAnimationStatesForNode(opaque, node);
    // Deferred image layers are the expensive case for multilingual scenes: the hidden placeholder
    // had no material/effect passes, so turning it visible changes graph topology and must rebuild
    // before any newly-created render targets or pipelines can contribute to the frame.
    if (mark_graph_dirty) opaque->scene->MarkRenderGraphTopologyDirty();
    LOG_VERBOSE("DeferredRuntimeImageRealize: materialized layer=%d topology-dirty=%s",
                layer_id,
                mark_graph_dirty ? "true" : "false");
    return true;
}

bool MaterializeDeferredTextLayerIfNeeded(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                                          bool mark_graph_dirty) {
    if (opaque == nullptr || opaque->scene == nullptr) return false;
    if (opaque->scene->deferredRuntimeTextLayerIds.count(layer_id) == 0) return true;

    const auto registration_range = CaptureSceneRegistrationRange(opaque);

    if (! wallpaper::MaterializeDeferredTextLayer(
            *opaque->scene, layer_id, &opaque->user_properties)) {
        LOG_ERROR("DeferredRuntimeTextRealize: failed for layer=%d", layer_id);
        return false;
    }

    auto* node = FindNodeById(opaque, layer_id);
    if (node == nullptr) {
        LOG_ERROR("DeferredRuntimeTextRealize: missing realized node for layer=%d", layer_id);
        return false;
    }

    RebindLayerRegistrations(opaque, layer_id, node);
    RegisterSceneRegistrationRange(opaque, registration_range);
    EnsureTextureAnimationStatesForNode(opaque, node);
    // Deferred text materialization follows the same contract as particles: the render graph built
    // while a hidden placeholder was present cannot draw the newly inserted text node until its
    // topology is rebuilt, even if ordinary uniforms or textures would only need a resource
    // refresh.
    if (mark_graph_dirty) opaque->scene->MarkRenderGraphTopologyDirty();
    LOG_VERBOSE("DeferredRuntimeTextRealize: materialized layer=%d topology-dirty=%s",
                layer_id,
                mark_graph_dirty ? "true" : "false");
    return true;
}

bool MaterializeDeferredVisibleLayerTreeIfNeeded(WPSceneScriptHost::Opaque* opaque,
                                                 int32_t                    root_layer_id) {
    if (opaque == nullptr || opaque->scene == nullptr || root_layer_id == 0) return false;

    std::vector<int32_t>        stack { root_layer_id };
    std::unordered_set<int32_t> visited;
    while (! stack.empty()) {
        const int32_t layer_id = stack.back();
        stack.pop_back();
        if (layer_id == 0 || ! visited.insert(layer_id).second) continue;

        if (! opaque->scene->IsLayerVisible(layer_id)) {
            // Deferred descendants inherit effective visibility from their parent chain. If this
            // layer is still effectively hidden after the current local-visible write, none of its
            // children can be visible either, so keep that branch logical and cheap.
            continue;
        }

        if (! MaterializeDeferredImageLayerIfNeeded(opaque, layer_id)) return false;
        if (! MaterializeDeferredParticleLayerIfNeeded(opaque, layer_id)) return false;
        if (! MaterializeDeferredTextLayerIfNeeded(opaque, layer_id)) return false;

        for (const auto child_id : opaque->scene->GetLayerChildren(layer_id)) {
            stack.push_back(child_id);
        }
    }

    return true;
}

struct LayerResidencyResources {
    std::unordered_set<std::string> static_textures;
    std::unordered_set<std::string> video_textures;
    std::unordered_set<std::string> render_targets;
};

bool RetainsGpuResidencyWhileHidden(const Scene& scene, int32_t layer_id) {
    if (layer_id == 0) return true;
    if (scene.IsLayerVisible(layer_id)) return true;
    return scene.offscreenDependencyLayerIds.count(layer_id) != 0;
}

void MergeResidencyResources(LayerResidencyResources& target,
                             const LayerResidencyResources& source) {
    target.static_textures.insert(source.static_textures.begin(), source.static_textures.end());
    target.video_textures.insert(source.video_textures.begin(), source.video_textures.end());
    target.render_targets.insert(source.render_targets.begin(), source.render_targets.end());
}

template<typename Callback>
void VisitLayerTree(const Scene& scene, int32_t root_layer_id, Callback&& callback) {
    std::vector<int32_t>        stack { root_layer_id };
    std::unordered_set<int32_t> visited;

    while (!stack.empty()) {
        const auto layer_id = stack.back();
        stack.pop_back();
        if (layer_id == 0 || !visited.insert(layer_id).second) continue;

        callback(layer_id);

        for (const auto child_id : scene.GetLayerChildren(layer_id)) {
            stack.push_back(child_id);
        }
    }
}

void PushUniqueResidencyNode(SceneNode* node, std::vector<SceneNode*>& nodes,
                             std::unordered_set<SceneNode*>& seen) {
    if (node == nullptr || !seen.insert(node).second) return;
    nodes.push_back(node);
}

void CollectLayerEffectResidencyNodes(const Scene& scene, int32_t layer_id,
                                      std::vector<SceneNode*>& nodes,
                                      std::unordered_set<SceneNode*>& seen) {
    auto camera_names_it = scene.objectRuntimeCameraNames.find(layer_id);
    if (camera_names_it == scene.objectRuntimeCameraNames.end()) return;

    for (const auto& camera_name : camera_names_it->second) {
        auto camera_it = scene.cameras.find(camera_name);
        if (camera_it == scene.cameras.end() || !camera_it->second ||
            !camera_it->second->HasImgEffect()) {
            continue;
        }

        auto* effect_layer = camera_it->second->GetImgEffect().get();
        if (effect_layer == nullptr) continue;

        if (effect_layer->HasFinalComposite()) {
            PushUniqueResidencyNode(&effect_layer->FinalNode(), nodes, seen);
        }

        for (size_t effect_index = 0; effect_index < effect_layer->EffectCount(); effect_index++) {
            auto& effect = effect_layer->GetEffect(effect_index);
            if (!effect) continue;
            for (auto& effect_node : effect->nodes) {
                PushUniqueResidencyNode(effect_node.sceneNode.get(), nodes, seen);
            }
        }
    }
}

std::vector<SceneNode*> CollectLayerResidencyNodes(const Scene& scene, int32_t layer_id) {
    std::vector<SceneNode*> nodes;
    std::unordered_set<SceneNode*> seen;

    if (auto runtime_nodes_it = scene.objectRuntimeNodes.find(layer_id);
        runtime_nodes_it != scene.objectRuntimeNodes.end()) {
        for (auto* node : runtime_nodes_it->second) {
            PushUniqueResidencyNode(node, nodes, seen);
        }
    }
    CollectLayerEffectResidencyNodes(scene, layer_id, nodes, seen);
    return nodes;
}

void CollectResidencyTextureKey(const Scene& scene, const std::string& key,
                                LayerResidencyResources& resources) {
    if (key.empty() || IsSpecLinkTex(key) || key == SpecTex_Default) return;

    if (scene.renderTargets.count(key) != 0 || IsSpecTex(key)) {
        resources.render_targets.insert(key);
        return;
    }

    if (auto texture_it = scene.textures.find(key);
        texture_it != scene.textures.end() && texture_it->second.isVideo) {
        resources.video_textures.insert(key);
        return;
    }

    resources.static_textures.insert(key);
}

void CollectResidencyMaterialResources(const Scene& scene, const SceneMaterial& material,
                                       LayerResidencyResources& resources) {
    for (const auto& key : material.textures) {
        CollectResidencyTextureKey(scene, key, resources);
    }
}

void CollectResidencyNodeResources(const Scene& scene, SceneNode* node,
                                   LayerResidencyResources& resources) {
    if (node == nullptr) return;

    if (auto* mesh = node->Mesh(); mesh != nullptr && mesh->Material() != nullptr) {
        CollectResidencyMaterialResources(scene, *mesh->Material(), resources);
    }

    if (auto* text = node->Text(); text != nullptr) {
        for (const auto& page : text->layout.glyph_pages) {
            if (!page.texture_key.empty()) resources.static_textures.insert(page.texture_key);
        }
    }
}

LayerResidencyResources CollectLayerResidencyResources(const Scene& scene, int32_t layer_id) {
    LayerResidencyResources resources;

    for (auto* node : CollectLayerResidencyNodes(scene, layer_id)) {
        CollectResidencyNodeResources(scene, node, resources);
    }

    if (auto render_targets_it = scene.objectRuntimeRenderTargets.find(layer_id);
        render_targets_it != scene.objectRuntimeRenderTargets.end()) {
        for (const auto& key : render_targets_it->second) {
            if (!key.empty() && key != SpecTex_Default) resources.render_targets.insert(key);
        }
    }

    return resources;
}

LayerResidencyResources CollectRetainedResidencyResources(
    const Scene& scene, const std::unordered_set<int32_t>& excluded_layers = {}) {
    LayerResidencyResources resources;
    std::unordered_set<int32_t> visited_layers;

    for (const auto& [layer_id, _] : scene.objectRuntimeNodes) {
        (void)_;
        if (!visited_layers.insert(layer_id).second) continue;
        if (excluded_layers.count(layer_id) != 0) continue;
        if (!RetainsGpuResidencyWhileHidden(scene, layer_id)) continue;

        MergeResidencyResources(resources, CollectLayerResidencyResources(scene, layer_id));
    }

    for (const auto& node : scene.bloom.nodes) {
        if (node) CollectResidencyNodeResources(scene, node.get(), resources);
    }
    if (scene.bloom.node) CollectResidencyNodeResources(scene, scene.bloom.node.get(), resources);

    return resources;
}

void QueueLayerResourceRelease(Scene& scene, int32_t layer_id,
                               const LayerResidencyResources& retained_resources,
                               const char* reason) {
    const auto resources = CollectLayerResidencyResources(scene, layer_id);
    std::size_t queued_static = 0;
    std::size_t queued_video = 0;
    std::size_t queued_render_targets = 0;

    for (const auto& key : resources.static_textures) {
        if (retained_resources.static_textures.count(key) != 0) continue;
        queued_static += scene.pendingStaticTextureReleaseKeys.insert(key).second ? 1 : 0;
    }
    for (const auto& key : resources.video_textures) {
        if (retained_resources.video_textures.count(key) != 0) continue;
        queued_video += scene.pendingVideoTextureReleaseKeys.insert(key).second ? 1 : 0;
    }
    for (const auto& key : resources.render_targets) {
        if (retained_resources.render_targets.count(key) != 0) continue;
        queued_render_targets += scene.pendingRenderTargetReleaseKeys.insert(key).second ? 1 : 0;
    }

    if (queued_static != 0 || queued_video != 0 || queued_render_targets != 0) {
        LOG_VERBOSE("SceneResidencyQueueRelease: reason=%s layer=%d static=%zu video=%zu "
                    "render-target=%zu",
                    reason != nullptr ? reason : "unknown",
                    layer_id,
                    queued_static,
                    queued_video,
                    queued_render_targets);
    }
}

void QueueHiddenLayerTreeResourceRelease(WPSceneScriptHost::Opaque* opaque,
                                         int32_t                    root_layer_id) {
    if (opaque == nullptr || opaque->scene == nullptr || root_layer_id == 0) return;

    auto& scene = *opaque->scene;
    const auto retained_resources = CollectRetainedResidencyResources(scene);

    std::size_t queued_static = 0;
    std::size_t queued_video = 0;
    std::size_t queued_render_targets = 0;

    VisitLayerTree(scene, root_layer_id, [&](int32_t layer_id) {
        if (!RetainsGpuResidencyWhileHidden(scene, layer_id)) {
            const auto static_before = scene.pendingStaticTextureReleaseKeys.size();
            const auto video_before = scene.pendingVideoTextureReleaseKeys.size();
            const auto render_target_before = scene.pendingRenderTargetReleaseKeys.size();
            QueueLayerResourceRelease(scene, layer_id, retained_resources, "hidden");
            queued_static += scene.pendingStaticTextureReleaseKeys.size() - static_before;
            queued_video += scene.pendingVideoTextureReleaseKeys.size() - video_before;
            queued_render_targets +=
                scene.pendingRenderTargetReleaseKeys.size() - render_target_before;
        }
    });

    if (queued_static != 0 || queued_video != 0 || queued_render_targets != 0) {
        LOG_VERBOSE("SceneResidencyQueueRelease: root-layer=%d static=%zu video=%zu render-target=%zu",
                    root_layer_id,
                    queued_static,
                    queued_video,
                    queued_render_targets);
    }
}

void CancelLayerResourceRelease(Scene& scene, int32_t layer_id, const char* reason,
                                std::size_t& cancelled_static,
                                std::size_t& cancelled_video,
                                std::size_t& cancelled_render_targets) {
    const auto resources = CollectLayerResidencyResources(scene, layer_id);
    std::size_t layer_static = 0;
    std::size_t layer_video = 0;
    std::size_t layer_render_targets = 0;
    for (const auto& key : resources.static_textures) {
        layer_static += scene.pendingStaticTextureReleaseKeys.erase(key);
    }
    for (const auto& key : resources.video_textures) {
        layer_video += scene.pendingVideoTextureReleaseKeys.erase(key);
    }
    for (const auto& key : resources.render_targets) {
        layer_render_targets += scene.pendingRenderTargetReleaseKeys.erase(key);
    }

    cancelled_static += layer_static;
    cancelled_video += layer_video;
    cancelled_render_targets += layer_render_targets;

    if (layer_static != 0 || layer_video != 0 || layer_render_targets != 0) {
        LOG_VERBOSE("SceneResidencyCancelRelease: reason=%s layer=%d static=%zu video=%zu "
                    "render-target=%zu",
                    reason != nullptr ? reason : "unknown",
                    layer_id,
                    layer_static,
                    layer_video,
                    layer_render_targets);
    }
}

void CancelLayerTreeResourceRelease(WPSceneScriptHost::Opaque* opaque,
                                    int32_t                    root_layer_id) {
    if (opaque == nullptr || opaque->scene == nullptr || root_layer_id == 0) return;

    auto& scene = *opaque->scene;
    std::size_t cancelled_static = 0;
    std::size_t cancelled_video = 0;
    std::size_t cancelled_render_targets = 0;

    VisitLayerTree(scene, root_layer_id, [&](int32_t layer_id) {
        // Visibility can flip more than once before the render thread drains pending releases. When
        // a hidden branch becomes visible again in the same frame, its resources are retained by the
        // new graph and must be removed from the eviction queue before Vulkan sees them. This is the
        // same generation-cancel idea mature engines use for streamed residency requests, just
        // scoped to the synchronous render-thread queue we already own.
        CancelLayerResourceRelease(scene,
                                   layer_id,
                                   "visible",
                                   cancelled_static,
                                   cancelled_video,
                                   cancelled_render_targets);
    });
}

std::optional<uint32_t> FindSoundHandleByLayerId(const WPSceneScriptHost::Opaque* opaque,
                                                 int32_t                          layer_id) {
    if (opaque == nullptr || opaque->scene == nullptr) return std::nullopt;
    auto it = opaque->scene->objectRuntimeSoundHandles.find(layer_id);
    if (it == opaque->scene->objectRuntimeSoundHandles.end()) return std::nullopt;
    return it->second;
}

std::optional<WPDynamicValue> ReadSoundPropertyValue(const WPSceneScriptHost::Opaque* opaque,
                                                     int32_t                          layer_id,
                                                     std::string_view property_name) {
    if (property_name != "volume") return std::nullopt;
    if (opaque == nullptr || opaque->scene == nullptr || opaque->scene->soundManager == nullptr) {
        return std::nullopt;
    }

    const auto sound_handle = FindSoundHandleByLayerId(opaque, layer_id);
    if (! sound_handle.has_value()) return std::nullopt;
    return WPDynamicValue(opaque->scene->soundManager->StreamVolume(*sound_handle));
}

bool ApplySoundPropertyValue(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                             std::string_view property_name, const WPDynamicValue& value) {
    if (property_name != "volume") return false;
    if (opaque == nullptr || opaque->scene == nullptr || opaque->scene->soundManager == nullptr) {
        return false;
    }

    const auto sound_handle = FindSoundHandleByLayerId(opaque, layer_id);
    if (! sound_handle.has_value()) return false;

    float volume = 0.0f;
    if (! value.tryGet(&volume)) return false;

    // Sound objects are not represented by SceneNode materials, so the live user-property path must
    // write straight into the mounted stream channel. This mirrors a cold parse where WPSoundParser
    // passes the resolved `volume` value directly to SoundManager::MountStream().
    const bool applied = opaque->scene->soundManager->SetStreamVolume(*sound_handle, volume);
    if (applied) {
        LOG_INFO("SceneSoundApply: layer=%d property='volume' value=%.3f handle=%u",
                 layer_id,
                 volume,
                 *sound_handle);
    }
    return applied;
}

TextLayerRuntimeState* FindTextLayerById(WPSceneScriptHost::Opaque* opaque, int32_t layer_id) {
    if (opaque == nullptr || opaque->scene == nullptr) return nullptr;
    auto it = opaque->scene->textLayers.find(layer_id);
    return it == opaque->scene->textLayers.end() ? nullptr : &it->second;
}

const TextLayerRuntimeState* FindTextLayerById(const WPSceneScriptHost::Opaque* opaque,
                                               int32_t                          layer_id) {
    if (opaque == nullptr || opaque->scene == nullptr) return nullptr;
    auto it = opaque->scene->textLayers.find(layer_id);
    return it == opaque->scene->textLayers.end() ? nullptr : &it->second;
}

std::optional<WPDynamicValue> ReadTextLayerPropertyValue(const WPSceneScriptHost::Opaque* opaque,
                                                         int32_t                          layer_id,
                                                         std::string_view property_name) {
    const auto* state = FindTextLayerById(opaque, layer_id);
    if (state == nullptr) return std::nullopt;
    const auto result = ReadTextLayerProperty(*state, property_name);
    // Runtime property reads happen before every registered script update. They are intentionally
    // silent unless a later mutation occurs, because per-frame "read current value" diagnostics
    // were producing more work than the text update itself during Clock minute rollovers.
    return result;
}

bool ApplyTextLayerPropertyValue(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                                 std::string_view property_name, const WPDynamicValue& value) {
    if (opaque == nullptr || opaque->scene == nullptr) return false;

    auto* state = FindTextLayerById(opaque, layer_id);
    if (state == nullptr) return false;

    if (property_name == "size") {
        std::array<float, 2> display_size {};
        if (! value.tryGet(&display_size)) return false;
        if (! wallpaper::ApplyTextLayerDisplaySize(*state, display_size)) {
            return false;
        }
    } else if (! wallpaper::ApplyTextLayerPropertyValue(*state, property_name, value)) {
        return false;
    }

    if (property_name == "name") {
        for (auto it = opaque->scene->layerNameToId.begin();
             it != opaque->scene->layerNameToId.end();) {
            if (it->second == layer_id) {
                it = opaque->scene->layerNameToId.erase(it);
            } else {
                ++it;
            }
        }
        opaque->scene->layerNameToId[state->object.name] = layer_id;
        if (auto* node = FindNodeById(opaque, layer_id)) node->SetName(state->object.name);
        return true;
    }

    if (opaque->scene->deferredRuntimeTextLayerIds.count(layer_id) != 0) {
        return true;
    }

    const auto update_strategy =
        wallpaper::ResolveTextLayerPropertyUpdateStrategy(*state, property_name);
    if (update_strategy == TextLayerPropertyUpdateStrategy::MaterialOnly) {
        if (auto* node = FindNodeById(opaque, layer_id); node != nullptr) {
            return SyncTextLayerSceneMaterials(*opaque->scene, layer_id);
        }
        return true;
    }

    if (update_strategy == TextLayerPropertyUpdateStrategy::TransformOnly) {
        if (! UpdateTextLayerSceneTransform(*opaque->scene, layer_id)) return false;
    } else if (update_strategy == TextLayerPropertyUpdateStrategy::BridgeResourceResize) {
        if (! UpdateTextLayerSceneBridgeResources(*opaque->scene, layer_id)) return false;
    } else if (! RebuildTextLayerSceneLayout(*opaque->scene, layer_id)) {
        return false;
    }

    // The dedicated text runtime actions now own the render-graph resource decision for text. They
    // distinguish cheap camera/mesh adjustments from the rare cases where an effect render target
    // truly needs more GPU capacity. Re-marking the graph dirty here would bring back the old
    // minute-boundary hitch by forcing every size change through the resource rebuild path.
    return true;
}

int32_t FindNodeId(const WPSceneScriptHost::Opaque* opaque, const SceneNode* node) {
    if (opaque == nullptr || opaque->scene == nullptr || node == nullptr) return 0;
    for (const auto& [id, candidate] : opaque->scene->layerNodes) {
        if (candidate == node) return id;
    }
    return 0;
}

int32_t FindOwningLayerId(const WPSceneScriptHost::Opaque* opaque, const SceneNode* node) {
    if (opaque == nullptr || opaque->scene == nullptr || node == nullptr) return 0;
    auto it = opaque->scene->nodeOwners.find(const_cast<SceneNode*>(node));
    return it == opaque->scene->nodeOwners.end() ? 0 : it->second;
}

bool IsDeferredRuntimeLayer(const WPSceneScriptHost::Opaque* opaque, int32_t layer_id) {
    if (opaque == nullptr || opaque->scene == nullptr || layer_id == 0) return false;

    // The script API is intentionally asked about the logical layer id instead of probing render
    // resources. Deferred image/text/particle placeholders all preserve their layer identity while
    // skipping heavy runtime nodes, which keeps this predicate independent from material layout.
    return opaque->scene->deferredRuntimeImageLayerIds.count(layer_id) != 0 ||
           opaque->scene->deferredRuntimeParticleLayerIds.count(layer_id) != 0 ||
           opaque->scene->deferredRuntimeTextLayerIds.count(layer_id) != 0;
}

void EnsureTextureAnimationStatesForNode(WPSceneScriptHost::Opaque* opaque, SceneNode* node) {
    if (opaque == nullptr || opaque->scene == nullptr || node == nullptr ||
        node->Mesh() == nullptr || node->Mesh()->Material() == nullptr) {
        return;
    }

    auto&       states   = opaque->texture_states[node];
    const auto& textures = node->Mesh()->Material()->textures;
    for (usize slot = 0; slot < textures.size(); slot++) {
        if (states.count(slot) != 0) continue;
        const auto& name = textures[slot];
        if (name.empty()) continue;
        if (opaque->scene->textures.count(name) == 0) continue;
        const auto& texture = opaque->scene->textures.at(name);
        if (! texture.isSprite) continue;
        states[slot] = TextureAnimationState {
            .base_animation = texture.spriteAnim,
            .animation      = texture.spriteAnim,
            .rate           = 1.0,
        };
    }

    if (states.empty()) {
        opaque->texture_states.erase(node);
    }
}

SceneNode* ResolveTextureAnimationNode(WPSceneScriptHost::Opaque* opaque, SceneNode* node,
                                       usize slot) {
    if (opaque == nullptr || node == nullptr) return nullptr;

    EnsureTextureAnimationStatesForNode(opaque, node);
    const auto direct_it = opaque->texture_states.find(node);
    if (direct_it != opaque->texture_states.end() && direct_it->second.count(slot) != 0) {
        return node;
    }

    const int32_t layer_id = FindOwningLayerId(opaque, node);
    if (layer_id == 0 || opaque->scene == nullptr) return nullptr;

    const auto runtime_nodes_it = opaque->scene->objectRuntimeNodes.find(layer_id);
    if (runtime_nodes_it == opaque->scene->objectRuntimeNodes.end()) return nullptr;

    for (auto* runtime_node : runtime_nodes_it->second) {
        if (runtime_node == nullptr) continue;
        EnsureTextureAnimationStatesForNode(opaque, runtime_node);
        const auto state_it = opaque->texture_states.find(runtime_node);
        if (state_it != opaque->texture_states.end() && state_it->second.count(slot) != 0) {
            return runtime_node;
        }
    }

    return nullptr;
}

void CollectVideoTextureKeysForNode(const WPSceneScriptHost::Opaque* opaque, SceneNode* node,
                                    std::vector<std::string>*        out_keys,
                                    std::unordered_set<std::string>* seen_keys) {
    if (opaque == nullptr || opaque->scene == nullptr || node == nullptr || out_keys == nullptr ||
        seen_keys == nullptr || node->Mesh() == nullptr || node->Mesh()->Material() == nullptr) {
        return;
    }

    for (const auto& key : node->Mesh()->Material()->textures) {
        if (key.empty() || seen_keys->count(key) != 0) continue;
        const auto texture_it = opaque->scene->textures.find(key);
        if (texture_it == opaque->scene->textures.end() || ! texture_it->second.isVideo) continue;
        seen_keys->insert(key);
        out_keys->push_back(key);
    }
}

std::vector<std::string> ResolveVideoTextureKeysForNode(WPSceneScriptHost::Opaque* opaque,
                                                        SceneNode*                 node) {
    std::vector<std::string> keys;
    if (opaque == nullptr || opaque->scene == nullptr || node == nullptr) return keys;

    std::unordered_set<std::string> seen_keys;
    CollectVideoTextureKeysForNode(opaque, node, &keys, &seen_keys);

    int32_t layer_id = FindOwningLayerId(opaque, node);
    if (layer_id == 0) layer_id = FindNodeId(opaque, node);
    if (layer_id == 0) return keys;

    const auto runtime_nodes_it = opaque->scene->objectRuntimeNodes.find(layer_id);
    if (runtime_nodes_it == opaque->scene->objectRuntimeNodes.end()) return keys;

    // Video layers can render through runtime child/source nodes instead of the authored layer
    // proxy node. Walk the whole runtime-node set so getVideoTexture() controls the decoder that
    // actually feeds the prepared custom-shader pass.
    for (auto* runtime_node : runtime_nodes_it->second) {
        CollectVideoTextureKeysForNode(opaque, runtime_node, &keys, &seen_keys);
    }

    return keys;
}

SceneNode* ResolvePuppetLayerNode(const WPSceneScriptHost::Opaque* opaque, SceneNode* node) {
    if (opaque == nullptr || node == nullptr) return nullptr;

    if (const auto* data = GetNodeData(opaque, node);
        data != nullptr && data->puppet_layer.hasPuppet()) {
        return node;
    }

    const int32_t layer_id = FindOwningLayerId(opaque, node);
    if (layer_id == 0 || opaque->scene == nullptr) return nullptr;

    const auto runtime_nodes_it = opaque->scene->objectRuntimeNodes.find(layer_id);
    if (runtime_nodes_it == opaque->scene->objectRuntimeNodes.end()) return nullptr;

    for (auto* runtime_node : runtime_nodes_it->second) {
        if (runtime_node == nullptr) continue;
        if (const auto* data = GetNodeData(opaque, runtime_node);
            data != nullptr && data->puppet_layer.hasPuppet()) {
            return runtime_node;
        }
    }

    return nullptr;
}

std::vector<SceneNode*> CollectPuppetLayerNodes(const WPSceneScriptHost::Opaque* opaque,
                                                SceneNode*                       node) {
    std::vector<SceneNode*> result;
    if (opaque == nullptr || node == nullptr || opaque->scene == nullptr) return result;

    std::unordered_set<SceneNode*> seen;
    auto                           add_if_puppet = [&](SceneNode* candidate) {
        if (candidate == nullptr || seen.count(candidate) != 0) return;
        if (const auto* data = GetNodeData(opaque, candidate);
            data != nullptr && data->puppet_layer.hasPuppet()) {
            seen.insert(candidate);
            result.push_back(candidate);
        }
    };

    // Effect-backed puppet layers are represented by several runtime nodes: the authored/world
    // node keeps layer identity for scripts, while one or more effect-pass nodes hold the puppet
    // instance that is actually rendered. Runtime animation-layer edits must be broadcast to every
    // copy so a live user-property toggle matches a cold parse with the same property value.
    add_if_puppet(node);

    int32_t layer_id = FindOwningLayerId(opaque, node);
    if (layer_id == 0) layer_id = FindNodeId(opaque, node);
    if (layer_id == 0) return result;

    if (const auto runtime_nodes_it = opaque->scene->objectRuntimeNodes.find(layer_id);
        runtime_nodes_it != opaque->scene->objectRuntimeNodes.end()) {
        for (auto* runtime_node : runtime_nodes_it->second) {
            add_if_puppet(runtime_node);
        }
    }

    if (const auto camera_names_it = opaque->scene->objectRuntimeCameraNames.find(layer_id);
        camera_names_it != opaque->scene->objectRuntimeCameraNames.end()) {
        for (const auto& camera_name : camera_names_it->second) {
            auto camera_it = opaque->scene->cameras.find(camera_name);
            if (camera_it == opaque->scene->cameras.end() || ! camera_it->second ||
                ! camera_it->second->HasImgEffect()) {
                continue;
            }

            auto& effect_layer = camera_it->second->GetImgEffect();
            if (! effect_layer) continue;

            add_if_puppet(&effect_layer->FinalNode());
            for (std::size_t effect_index = 0; effect_index < effect_layer->EffectCount();
                 effect_index++) {
                auto& effect = effect_layer->GetEffect(effect_index);
                for (auto& effect_node : effect->nodes) {
                    add_if_puppet(effect_node.sceneNode.get());
                }
            }
        }
    }

    return result;
}

std::optional<int32_t> ResolveLayerReference(JSContext* context, WPSceneScriptHost::Opaque* opaque,
                                             JSValueConst value) {
    if (opaque == nullptr || opaque->scene == nullptr) return std::nullopt;

    int32_t layer_id = 0;
    if (JS_ToInt32(context, &layer_id, value) == 0) {
        if (opaque->scene->layerNodes.count(layer_id) != 0) return layer_id;
    }

    std::string layer_name;
    if (ReadJSString(context, value, &layer_name)) {
        auto it = opaque->scene->layerNameToId.find(layer_name);
        if (it != opaque->scene->layerNameToId.end()) return it->second;
    }

    if (JS_IsObject(value)) {
        JSValue node_id_value = JS_GetPropertyStr(context, value, "__nodeId");
        if (! JS_IsException(node_id_value)) {
            int32_t node_id = 0;
            if (JS_ToInt32(context, &node_id, node_id_value) == 0 &&
                opaque->scene->layerNodes.count(node_id) != 0) {
                JS_FreeValue(context, node_id_value);
                return node_id;
            }
        }
        JS_FreeValue(context, node_id_value);
    }

    return std::nullopt;
}

std::shared_ptr<SceneNode> ExtractChildNode(SceneNode* parent, SceneNode* child) {
    if (parent == nullptr || child == nullptr) return nullptr;
    auto& children = parent->GetChildren();
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (it->get() != child) continue;
        auto extracted = *it;
        children.erase(it);
        return extracted;
    }
    return nullptr;
}

SceneNode* GetLogicalParentNode(const WPSceneScriptHost::Opaque* opaque, SceneNode* node) {
    if (opaque == nullptr || opaque->scene == nullptr || node == nullptr) return nullptr;
    const int32_t layer_id = FindNodeId(opaque, node);
    if (layer_id == 0) return nullptr;
    const auto binding = opaque->scene->GetLayerParentBinding(layer_id);
    if (binding.parent_id == 0) return nullptr;
    return FindNodeById(const_cast<WPSceneScriptHost::Opaque*>(opaque), binding.parent_id);
}

std::shared_ptr<SceneNode> ExtractNodeFromSceneGraph(WPSceneScriptHost::Opaque* opaque,
                                                     SceneNode*                 node) {
    if (opaque == nullptr || opaque->scene == nullptr || opaque->scene->sceneGraph == nullptr ||
        node == nullptr) {
        return nullptr;
    }

    if (auto* parent = node->Parent(); parent != nullptr) {
        return ExtractChildNode(parent, node);
    }
    return ExtractChildNode(opaque->scene->sceneGraph.get(), node);
}

bool IsLogicalAncestor(const WPSceneScriptHost::Opaque* opaque, SceneNode* node,
                       SceneNode* ancestor) {
    if (opaque == nullptr || opaque->scene == nullptr || node == nullptr || ancestor == nullptr)
        return false;
    const int32_t ancestor_id = FindNodeId(opaque, ancestor);
    int32_t       current_id  = FindNodeId(opaque, node);
    while (current_id != 0) {
        const auto binding = opaque->scene->GetLayerParentBinding(current_id);
        current_id         = binding.parent_id;
        if (current_id == ancestor_id) return true;
    }
    return false;
}

struct ScriptAttachmentBinding {
    uint32_t        bone_index { 0xFFFFFFFFu };
    Eigen::Affine3f anchor_transform { Eigen::Affine3f::Identity() };
    std::string     attachment_name;
};

std::optional<ScriptAttachmentBinding>
ResolveAttachmentBindingForScript(JSContext* context, JSValueConst value, const WPPuppet& puppet) {
    if (JS_IsUndefined(value) || JS_IsNull(value)) return std::nullopt;

    if (JS_IsNumber(value)) {
        int32_t numeric = -1;
        if (JS_ToInt32(context, &numeric, value) != 0 || numeric < 0) return std::nullopt;
        const auto index = static_cast<size_t>(numeric);
        if (index < puppet.attachments.size()) {
            return ScriptAttachmentBinding {
                .bone_index       = puppet.attachments[index].bone_index,
                .anchor_transform = puppet.attachments[index].transform,
                .attachment_name  = std::to_string(numeric),
            };
        }
        if (index < puppet.bones.size()) {
            return ScriptAttachmentBinding {
                .bone_index      = static_cast<uint32_t>(numeric),
                .attachment_name = std::to_string(numeric),
            };
        }
        return std::nullopt;
    }

    std::string name;
    if (! ReadJSString(context, value, &name)) return std::nullopt;
    if (const auto* attachment = puppet.FindAttachment(name)) {
        return ScriptAttachmentBinding {
            .bone_index       = attachment->bone_index,
            .anchor_transform = attachment->transform,
            .attachment_name  = name,
        };
    }
    const auto bone_index = puppet.FindBoneIndex(name);
    if (bone_index == 0xFFFFFFFFu) return std::nullopt;
    return ScriptAttachmentBinding {
        .bone_index      = bone_index,
        .attachment_name = name,
    };
}

bool AttachNodeSharedPtrToParent(WPSceneScriptHost::Opaque*        opaque,
                                 const std::shared_ptr<SceneNode>& node, SceneNode* parent) {
    if (opaque == nullptr || opaque->scene == nullptr || opaque->scene->sceneGraph == nullptr ||
        ! node) {
        return false;
    }
    if (parent != nullptr) {
        parent->AppendChild(node);
    } else {
        opaque->scene->sceneGraph->AppendChild(node);
    }
    return true;
}

bool RebindLayerParent(WPSceneScriptHost::Opaque* opaque, SceneNode* node, SceneNode* new_parent,
                       std::optional<ScriptAttachmentBinding> attachment_binding,
                       bool                                   adjust_transforms) {
    if (opaque == nullptr || opaque->scene == nullptr || node == nullptr) return false;
    const int32_t layer_id = FindNodeId(opaque, node);
    if (layer_id == 0) return false;

    auto*           data          = GetNodeData(opaque, node);
    Eigen::Affine3f current_world = Eigen::Affine3f::Identity();
    node->UpdateTrans();
    current_world.matrix() = node->ModelTrans().cast<float>();

    const Eigen::Affine3f          local_before(node->GetLocalTrans().cast<float>());
    std::optional<Eigen::Matrix4d> attachment_world;
    if (attachment_binding.has_value()) {
        if (new_parent == nullptr || data == nullptr) return false;
        attachment_world =
            GetAttachmentWorldTransform(opaque,
                                        new_parent,
                                        WPPuppet::Attachment {
                                            .bone_index = attachment_binding->bone_index,
                                            .transform  = attachment_binding->anchor_transform,
                                        });
        if (! attachment_world.has_value()) return false;
    }

    auto detached_node = ExtractNodeFromSceneGraph(opaque, node);
    if (! detached_node) return false;

    if (data != nullptr) {
        data->transform_binding = {};
        data->parallax_anchor   = nullptr;
    }

    Eigen::Affine3f updated_local = local_before;
    SceneNode*      scene_parent  = nullptr;

    if (attachment_binding.has_value()) {
        if (adjust_transforms) {
            updated_local = Eigen::Affine3f(
                (attachment_world->inverse() * current_world.matrix().cast<double>())
                    .cast<float>());
        }

        data->AttachToBone(new_parent,
                           attachment_binding->bone_index,
                           attachment_binding->anchor_transform,
                           updated_local);
        scene_parent = nullptr;
    } else if (new_parent != nullptr) {
        if (data != nullptr) {
            if (adjust_transforms) {
                new_parent->UpdateTrans();
                updated_local = Eigen::Affine3f(
                    (new_parent->ModelTrans().inverse() * current_world.matrix().cast<double>())
                        .cast<float>());
            }
            data->InheritParentTransform(new_parent);
            scene_parent = nullptr;
        } else {
            if (adjust_transforms) {
                new_parent->UpdateTrans();
                updated_local = Eigen::Affine3f(
                    (new_parent->ModelTrans().inverse() * current_world.matrix().cast<double>())
                        .cast<float>());
            }
            scene_parent = new_parent;
        }
    } else {
        if (adjust_transforms) {
            updated_local = current_world;
        }
        scene_parent = nullptr;
    }

    node->SetLocalAffine(updated_local);
    if (! AttachNodeSharedPtrToParent(opaque, detached_node, scene_parent)) return false;
    const int32_t parent_layer_id = new_parent != nullptr ? FindNodeId(opaque, new_parent) : 0;
    opaque->scene->SetLayerParentBinding(
        layer_id,
        parent_layer_id,
        attachment_binding.has_value() ? attachment_binding->attachment_name : std::string {});
    opaque->scene->ApplyLayerVisibility(layer_id);
    opaque->scene->MarkRenderGraphTopologyDirty();
    ResortLayerTree(opaque->scene->sceneGraph.get(), opaque);
    return true;
}

void CollectNodeSubtree(SceneNode* node, std::unordered_set<SceneNode*>& out_nodes) {
    if (node == nullptr || out_nodes.contains(node)) return;
    out_nodes.insert(node);
    for (const auto& child : node->GetChildren()) {
        CollectNodeSubtree(child.get(), out_nodes);
    }
}

void FreeScriptInstance(JSContext* context, ScriptInstance& instance) {
    if (context == nullptr) return;
    if (instance.initialized && ! JS_IsUndefined(instance.destroy_fn)) {
        JSValue result = JS_Call(context, instance.destroy_fn, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(result)) {
            LogQuickJSException(context, "destroy");
        }
        JS_FreeValue(context, result);
    }

    FreeJSValue(context, instance.script_properties);
    FreeJSValue(context, instance.exports);
    FreeJSValue(context, instance.init_fn);
    FreeJSValue(context, instance.update_fn);
    FreeJSValue(context, instance.apply_user_properties_fn);
    FreeJSValue(context, instance.apply_general_settings_fn);
    FreeJSValue(context, instance.cursor_enter_fn);
    FreeJSValue(context, instance.cursor_leave_fn);
    FreeJSValue(context, instance.cursor_move_fn);
    FreeJSValue(context, instance.cursor_down_fn);
    FreeJSValue(context, instance.cursor_up_fn);
    FreeJSValue(context, instance.cursor_click_fn);
    FreeJSValue(context, instance.media_thumbnail_changed_fn);
    FreeJSValue(context, instance.media_properties_changed_fn);
    FreeJSValue(context, instance.media_playback_changed_fn);
    FreeJSValue(context, instance.destroy_fn);
    FreeJSValue(context, instance.resize_screen_fn);
}

void ResortChildLayers(SceneNode* parent, const WPSceneScriptHost::Opaque* opaque) {
    if (parent == nullptr || opaque == nullptr || opaque->scene == nullptr) return;

    auto& children = parent->GetChildren();
    if (children.size() < 2) return;

    std::unordered_map<int32_t, size_t> order_index;
    for (size_t index = 0; index < opaque->scene->layerOrder.size(); index++) {
        order_index[opaque->scene->layerOrder[index]] = index;
    }

    std::vector<std::shared_ptr<SceneNode>> original_children(children.begin(), children.end());
    std::vector<bool>                       orderable_slots;
    std::vector<std::shared_ptr<SceneNode>> orderable_children;
    orderable_slots.reserve(original_children.size());
    for (const auto& child : original_children) {
        const auto owner_id  = FindOwningLayerId(opaque, child.get());
        const bool orderable = owner_id > 0 && order_index.contains(owner_id);
        orderable_slots.push_back(orderable);
        if (orderable) orderable_children.push_back(child);
    }

    if (orderable_children.size() < 2) return;

    std::stable_sort(orderable_children.begin(),
                     orderable_children.end(),
                     [&order_index, &opaque](const auto& lhs, const auto& rhs) {
                         const auto lhs_owner = FindOwningLayerId(opaque, lhs.get());
                         const auto rhs_owner = FindOwningLayerId(opaque, rhs.get());
                         const auto lhs_it    = order_index.find(lhs_owner);
                         const auto rhs_it    = order_index.find(rhs_owner);
                         const auto lhs_order = lhs_it == order_index.end()
                                                    ? std::numeric_limits<size_t>::max()
                                                    : lhs_it->second;
                         const auto rhs_order = rhs_it == order_index.end()
                                                    ? std::numeric_limits<size_t>::max()
                                                    : rhs_it->second;
                         return lhs_order < rhs_order;
                     });

    std::list<std::shared_ptr<SceneNode>> reordered;
    size_t                                orderable_index = 0;
    for (size_t index = 0; index < original_children.size(); index++) {
        if (orderable_slots[index]) {
            reordered.push_back(orderable_children[orderable_index++]);
        } else {
            reordered.push_back(original_children[index]);
        }
    }
    children = std::move(reordered);
}

void ResortLayerTree(SceneNode* parent, const WPSceneScriptHost::Opaque* opaque) {
    if (parent == nullptr) return;
    ResortChildLayers(parent, opaque);
    for (const auto& child : parent->GetChildren()) {
        ResortLayerTree(child.get(), opaque);
    }
}

void ProcessPendingSceneLayerDestroy(WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr || opaque->scene == nullptr || opaque->runtime.context == nullptr ||
        opaque->pending_destroy_layer_ids.empty()) {
        return;
    }

    JSContext*           context = opaque->runtime.context;
    std::vector<int32_t> pending = std::move(opaque->pending_destroy_layer_ids);
    opaque->pending_destroy_layer_ids.clear();

    std::unordered_set<int32_t> destroying_layers(pending.begin(), pending.end());
    const auto retained_resources =
        CollectRetainedResidencyResources(*opaque->scene, destroying_layers);

    for (const int32_t layer_id : pending) {
        // Dynamic deletion used to get GPU cleanup as an accidental side effect of the broad
        // topology-rebuild cache clear. Topology rebuilds are now cache-preserving, so delete must
        // explicitly queue only the resources owned by the removed layer and not by another still
        // retained layer.
        QueueLayerResourceRelease(*opaque->scene, layer_id, retained_resources, "destroy");

        auto runtime_nodes_it = opaque->scene->objectRuntimeNodes.find(layer_id);

        std::unordered_set<SceneNode*>          destroyed_nodes;
        std::vector<std::shared_ptr<SceneNode>> detached_roots;
        if (runtime_nodes_it != opaque->scene->objectRuntimeNodes.end()) {
            for (SceneNode* runtime_node : runtime_nodes_it->second) {
                if (runtime_node == nullptr) continue;
                CollectNodeSubtree(runtime_node, destroyed_nodes);
                if (auto* parent = runtime_node->Parent()) {
                    if (auto detached = ExtractChildNode(parent, runtime_node)) {
                        detached_roots.push_back(std::move(detached));
                    }
                }
            }
        }

        std::unordered_set<uint32_t> removed_instance_ids;
        auto&                        instances = opaque->instances;
        instances.erase(std::remove_if(instances.begin(),
                                       instances.end(),
                                       [&](const std::unique_ptr<ScriptInstance>& instance) {
                                           if (! instance || ! destroyed_nodes.contains(
                                                                 instance->registration.node)) {
                                               return false;
                                           }
                                           removed_instance_ids.insert(instance->instance_id);
                                           FreeScriptInstance(context, *instance);
                                           return true;
                                       }),
                        instances.end());

        auto& timers = opaque->timers;
        timers.erase(
            std::remove_if(timers.begin(),
                           timers.end(),
                           [&](ScriptTimer& timer) {
                               if (! removed_instance_ids.contains(timer.owner_instance_id))
                                   return false;
                               FreeJSValue(context, timer.callback);
                               return true;
                           }),
            timers.end());

        opaque->property_animations.erase(
            std::remove_if(opaque->property_animations.begin(),
                           opaque->property_animations.end(),
                           [&](const PropertyAnimationInstance& animation) {
                               return destroyed_nodes.contains(animation.registration.node);
                           }),
            opaque->property_animations.end());

        for (SceneNode* node : destroyed_nodes) {
            opaque->texture_states.erase(node);
            auto animation_state_it = opaque->animation_layer_states.find(node);
            if (animation_state_it != opaque->animation_layer_states.end()) {
                for (auto& [index, state] : animation_state_it->second) {
                    (void)index;
                    for (auto& callback : state.ended_callbacks) {
                        FreeJSValue(context, callback);
                    }
                }
                opaque->animation_layer_states.erase(animation_state_it);
            }
            opaque->scene->nodeOwners.erase(node);
        }

        opaque->scene->objectRuntimeNodes.erase(layer_id);
        opaque->scene->ClearLayerParentBinding(layer_id);
        opaque->scene->layerLocalVisibility.erase(layer_id);
        if (auto sound_it = opaque->scene->objectRuntimeSoundHandles.find(layer_id);
            sound_it != opaque->scene->objectRuntimeSoundHandles.end()) {
            if (opaque->scene->soundManager != nullptr) {
                opaque->scene->soundManager->UnmountStream(sound_it->second);
            }
            opaque->scene->objectRuntimeSoundHandles.erase(sound_it);
        }
        if (auto camera_names_it = opaque->scene->objectRuntimeCameraNames.find(layer_id);
            camera_names_it != opaque->scene->objectRuntimeCameraNames.end()) {
            for (const auto& camera_name : camera_names_it->second) {
                for (auto& [linked_name, linked_cameras] : opaque->scene->linkedCameras) {
                    linked_cameras.erase(
                        std::remove(linked_cameras.begin(), linked_cameras.end(), camera_name),
                        linked_cameras.end());
                }
                opaque->scene->cameras.erase(camera_name);
            }
            opaque->scene->objectRuntimeCameraNames.erase(camera_names_it);
        }
        if (auto render_targets_it = opaque->scene->objectRuntimeRenderTargets.find(layer_id);
            render_targets_it != opaque->scene->objectRuntimeRenderTargets.end()) {
            for (const auto& render_target : render_targets_it->second) {
                opaque->scene->renderTargets.erase(render_target);
            }
            opaque->scene->objectRuntimeRenderTargets.erase(render_targets_it);
        }
        if (auto lights_it = opaque->scene->objectRuntimeLights.find(layer_id);
            lights_it != opaque->scene->objectRuntimeLights.end()) {
            auto& lights = opaque->scene->lights;
            lights.erase(std::remove_if(lights.begin(),
                                        lights.end(),
                                        [&](const std::unique_ptr<SceneLight>& light) {
                                            return light != nullptr &&
                                                   std::find(lights_it->second.begin(),
                                                             lights_it->second.end(),
                                                             light.get()) !=
                                                       lights_it->second.end();
                                        }),
                         lights.end());
            opaque->scene->objectRuntimeLights.erase(lights_it);
        }
        if (auto particle_it = opaque->scene->objectRuntimeParticleSubsystems.find(layer_id);
            particle_it != opaque->scene->objectRuntimeParticleSubsystems.end()) {
            auto& subsystems = opaque->scene->paritileSys->subsystems;
            subsystems.erase(
                std::remove_if(subsystems.begin(),
                               subsystems.end(),
                               [&](const std::unique_ptr<ParticleSubSystem>& subsystem) {
                                   return subsystem != nullptr &&
                                          std::find(particle_it->second.begin(),
                                                    particle_it->second.end(),
                                                    subsystem.get()) != particle_it->second.end();
                               }),
                subsystems.end());
            opaque->scene->objectRuntimeParticleSubsystems.erase(particle_it);
        }
        if (auto text_it = opaque->scene->textLayers.find(layer_id);
            text_it != opaque->scene->textLayers.end()) {
            // First-class text primitives own their atlas pages directly and the dedicated text
            // pass uploads those images from the primitive itself. Deleting a text layer
            // therefore only needs to drop the runtime registry entry; there is no synthetic
            // parser-backed authoritative text image left to unregister here.
            opaque->scene->textLayers.erase(text_it);
        }
        opaque->scene->imageLayers.erase(layer_id);
        // Deferred runtime sets are lightweight ownership records for hidden placeholder layers.
        // Delete must clear every deferred kind together with the regular registries so a later
        // dynamic layer that reuses this authored id cannot inherit a stale materialization state.
        opaque->scene->deferredRuntimeImageLayerIds.erase(layer_id);
        opaque->scene->deferredRuntimeParticleLayerIds.erase(layer_id);
        opaque->scene->deferredRuntimeTextLayerIds.erase(layer_id);
        opaque->scene->layerNodes.erase(layer_id);
        opaque->scene->initialLayerConfigJson.erase(layer_id);
        opaque->scene->scriptRegistrations.erase(
            std::remove_if(opaque->scene->scriptRegistrations.begin(),
                           opaque->scene->scriptRegistrations.end(),
                           [layer_id](const WPSceneScriptRegistration& registration) {
                               return registration.object_id == layer_id;
                           }),
            opaque->scene->scriptRegistrations.end());
        opaque->scene->propertyAnimationRegistrations.erase(
            std::remove_if(opaque->scene->propertyAnimationRegistrations.begin(),
                           opaque->scene->propertyAnimationRegistrations.end(),
                           [layer_id](const WPSceneScriptRegistration& registration) {
                               return registration.object_id == layer_id;
                           }),
            opaque->scene->propertyAnimationRegistrations.end());
        opaque->scene->bindingRegistrations.erase(
            std::remove_if(opaque->scene->bindingRegistrations.begin(),
                           opaque->scene->bindingRegistrations.end(),
                           [layer_id](const WPSceneScriptRegistration& registration) {
                               return registration.object_id == layer_id;
                           }),
            opaque->scene->bindingRegistrations.end());
        opaque->property_bindings.erase(
            std::remove_if(opaque->property_bindings.begin(),
                           opaque->property_bindings.end(),
                           [layer_id](const WPSceneScriptRegistration& registration) {
                               return registration.object_id == layer_id;
                           }),
            opaque->property_bindings.end());
        opaque->scene->layerOrder.erase(std::remove(opaque->scene->layerOrder.begin(),
                                                    opaque->scene->layerOrder.end(),
                                                    layer_id),
                                        opaque->scene->layerOrder.end());
        for (auto it = opaque->scene->layerNameToId.begin();
             it != opaque->scene->layerNameToId.end();) {
            if (it->second == layer_id) {
                it = opaque->scene->layerNameToId.erase(it);
            } else {
                ++it;
            }
        }
        opaque->scene->MarkRenderGraphTopologyDirty();
    }

    ResortLayerTree(opaque->scene->sceneGraph.get(), opaque);
}

void TraverseSceneNodes(SceneNode* node, const std::function<void(SceneNode*)>& visitor) {
    if (node == nullptr) return;
    visitor(node);
    for (const auto& child : node->GetChildren()) {
        TraverseSceneNodes(child.get(), visitor);
    }
}

void UpdateSceneLightingUniforms(WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr || opaque->scene == nullptr || opaque->scene->sceneGraph == nullptr)
        return;

    const auto ambient  = opaque->scene->ambientColor;
    const auto skylight = opaque->scene->skylightColor;
    TraverseSceneNodes(opaque->scene->sceneGraph.get(), [&](SceneNode* node) {
        if (node == nullptr || node->Mesh() == nullptr || node->Mesh()->Material() == nullptr)
            return;
        auto& const_values                   = node->Mesh()->Material()->customShader.constValues;
        const_values["g_LightAmbientColor"]  = ambient;
        const_values["g_LightSkylightColor"] = skylight;
    });
}

void ApplySceneCameraParallax(WPSceneScriptHost::Opaque* opaque) {
    auto* updater = GetShaderUpdater(opaque);
    if (opaque == nullptr || opaque->scene == nullptr || updater == nullptr) return;

    updater->SetCameraParallax(WPCameraParallax {
        .enable         = opaque->scene->cameraParallax,
        .amount         = opaque->scene->cameraParallaxAmount,
        .delay          = opaque->scene->cameraParallaxDelay,
        .mouseinfluence = opaque->scene->cameraParallaxMouseInfluence,
    });
}

const SceneCamera* GetPerspectiveSceneCamera(const WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr || opaque->scene == nullptr) return nullptr;
    auto it = opaque->scene->cameras.find("global_perspective");
    return it == opaque->scene->cameras.end() ? nullptr : it->second.get();
}

SceneCamera* GetPerspectiveSceneCamera(WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr || opaque->scene == nullptr) return nullptr;
    auto it = opaque->scene->cameras.find("global_perspective");
    return it == opaque->scene->cameras.end() ? nullptr : it->second.get();
}

const Scene::ImageLayerRuntimeState* FindImageLayerById(const WPSceneScriptHost::Opaque* opaque,
                                                        int32_t                          layer_id) {
    if (opaque == nullptr || opaque->scene == nullptr) return nullptr;
    auto it = opaque->scene->imageLayers.find(layer_id);
    return it == opaque->scene->imageLayers.end() ? nullptr : std::addressof(it->second);
}

Scene::ImageLayerRuntimeState* FindImageLayerById(WPSceneScriptHost::Opaque* opaque,
                                                  int32_t                    layer_id) {
    if (opaque == nullptr || opaque->scene == nullptr) return nullptr;
    auto it = opaque->scene->imageLayers.find(layer_id);
    return it == opaque->scene->imageLayers.end() ? nullptr : std::addressof(it->second);
}

const WPPuppetLayer::AnimationLayer*
FindAnimationLayerState(const WPSceneScriptHost::Opaque* opaque, SceneNode* node, usize index) {
    node = ResolvePuppetLayerNode(opaque, node);
    if (node == nullptr) return nullptr;
    const auto* data = GetNodeData(opaque, node);
    if (data == nullptr || ! data->puppet_layer.hasPuppet()) return nullptr;
    return data->puppet_layer.AnimationLayerState(index);
}

WPPuppetLayer::AnimationLayer* FindAnimationLayerState(WPSceneScriptHost::Opaque* opaque,
                                                       SceneNode* node, usize index) {
    node = ResolvePuppetLayerNode(opaque, node);
    if (node == nullptr) return nullptr;
    auto* data = GetNodeData(opaque, node);
    if (data == nullptr || ! data->puppet_layer.hasPuppet()) return nullptr;
    return data->puppet_layer.AnimationLayerState(index);
}

const WPPuppet::Animation* FindAnimationDefinition(const WPSceneScriptHost::Opaque* opaque,
                                                   SceneNode* node, usize index) {
    node = ResolvePuppetLayerNode(opaque, node);
    if (node == nullptr) return nullptr;
    const auto* data = GetNodeData(opaque, node);
    if (data == nullptr || ! data->puppet_layer.hasPuppet()) return nullptr;
    return data->puppet_layer.AnimationDefinition(index);
}

WPShaderValueData* GetMutableNodeData(const WPSceneScriptHost::Opaque* opaque, SceneNode* node) {
    return GetNodeData(const_cast<WPSceneScriptHost::Opaque*>(opaque), node);
}

const WPPuppet* AdvanceNodePuppetForScriptQuery(const WPSceneScriptHost::Opaque* opaque,
                                                SceneNode*                       node) {
    auto* data = GetMutableNodeData(opaque, node);
    if (data == nullptr || ! data->puppet_layer.hasPuppet()) return nullptr;

    const auto next_serial = [&]() -> uint64_t {
        auto* updater = GetShaderUpdater(opaque);
        return updater != nullptr ? updater->NextPuppetFrameSerial() : 1u;
    }();
    const double frame_time =
        opaque != nullptr && opaque->scene != nullptr ? opaque->scene->frameTime : 0.0;
    data->puppet_layer.AdvanceIfNeeded(frame_time, next_serial);
    return data->puppet_layer.Puppet();
}

struct AttachmentReference {
    uint32_t                    index { 0xFFFFFFFFu };
    const WPPuppet::Attachment* attachment { nullptr };
};

std::optional<AttachmentReference>
ResolveAttachmentReference(JSContext* context, JSValueConst value, const WPPuppet& puppet) {
    if (JS_IsNumber(value)) {
        int32_t attachment_index = -1;
        if (JS_ToInt32(context, &attachment_index, value) == 0 && attachment_index >= 0 &&
            static_cast<size_t>(attachment_index) < puppet.attachments.size()) {
            return AttachmentReference {
                .index      = static_cast<uint32_t>(attachment_index),
                .attachment = &puppet.attachments[static_cast<size_t>(attachment_index)],
            };
        }
    }

    std::string name;
    if (! ReadJSString(context, value, &name)) return std::nullopt;
    for (uint32_t index = 0; index < puppet.attachments.size(); index++) {
        if (puppet.attachments[index].name == name) {
            return AttachmentReference {
                .index      = index,
                .attachment = &puppet.attachments[index],
            };
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> ResolveBoneReference(JSContext* context, JSValueConst value,
                                             const WPPuppet& puppet) {
    if (JS_IsNumber(value)) {
        int32_t bone_index = -1;
        if (JS_ToInt32(context, &bone_index, value) == 0 && bone_index >= 0 &&
            static_cast<size_t>(bone_index) < puppet.bones.size()) {
            return static_cast<uint32_t>(bone_index);
        }
    }

    std::string name;
    if (! ReadJSString(context, value, &name)) return std::nullopt;
    const auto bone_index = puppet.FindBoneIndex(name);
    return bone_index == 0xFFFFFFFFu ? std::nullopt : std::optional<uint32_t>(bone_index);
}

std::array<double, 3> AffineTranslation(const Eigen::Affine3f& affine) {
    const auto translation = affine.translation();
    return {
        static_cast<double>(translation.x()),
        static_cast<double>(translation.y()),
        static_cast<double>(translation.z()),
    };
}

std::array<double, 3> AffineEulerAngles(const Eigen::Affine3f& affine) {
    Eigen::Matrix3f linear = affine.linear();
    for (int axis = 0; axis < 3; axis++) {
        const float length = linear.col(axis).norm();
        if (length > 1e-6f) {
            linear.col(axis) /= length;
        } else {
            linear.col(axis).setZero();
            linear(axis, axis) = 1.0f;
        }
    }

    const auto zyx = linear.eulerAngles(2, 1, 0);
    return {
        static_cast<double>(zyx[2]),
        static_cast<double>(zyx[1]),
        static_cast<double>(zyx[0]),
    };
}

Eigen::Affine3f ComposeAffine(const Eigen::Vector3f& translation, const Eigen::Vector3f& rotation,
                              const Eigen::Vector3f& scale) {
    Eigen::Affine3f affine = Eigen::Affine3f::Identity();
    affine.prescale(scale);
    affine.prerotate(Eigen::AngleAxisf(rotation.x(), Eigen::Vector3f::UnitX()));
    affine.prerotate(Eigen::AngleAxisf(rotation.y(), Eigen::Vector3f::UnitY()));
    affine.prerotate(Eigen::AngleAxisf(rotation.z(), Eigen::Vector3f::UnitZ()));
    affine.pretranslate(translation);
    return affine;
}

void DecomposeAffine(const Eigen::Affine3f& affine, Eigen::Vector3f& translation,
                     Eigen::Vector3f& rotation, Eigen::Vector3f& scale) {
    Eigen::Matrix3f linear = affine.linear();
    scale = Eigen::Vector3f(linear.col(0).norm(), linear.col(1).norm(), linear.col(2).norm());
    for (int axis = 0; axis < 3; axis++) {
        if (scale[axis] > 1e-6f) {
            linear.col(axis) /= scale[axis];
        } else {
            linear.col(axis).setZero();
            linear(axis, axis) = 1.0f;
            scale[axis]        = 1.0f;
        }
    }

    const auto zyx = linear.eulerAngles(2, 1, 0);
    translation    = affine.translation();
    rotation       = Eigen::Vector3f(zyx[2], zyx[1], zyx[0]);
}

NodeScaleSnapshot CaptureNodeScale(SceneNode* node) {
    NodeScaleSnapshot snapshot;
    if (node == nullptr) return snapshot;

    node->UpdateTrans();
    snapshot.local = node->Scale();

    Eigen::Vector3f translation;
    Eigen::Vector3f rotation;
    DecomposeAffine(
        Eigen::Affine3f(node->ModelTrans().cast<float>()), translation, rotation, snapshot.world);
    return snapshot;
}

std::optional<Eigen::Matrix4d> ReadMatrix4FromJS(JSContext* context, JSValueConst value) {
    if (JS_IsException(value) || JS_IsUndefined(value) || JS_IsNull(value)) return std::nullopt;

    Eigen::Matrix4d matrix = Eigen::Matrix4d::Identity();
    for (uint32_t index = 0; index < 16; index++) {
        JSValue    item   = JS_GetPropertyUint32(context, value, index);
        double     number = 0.0;
        const bool ok     = ! JS_IsException(item) && ReadJSNumber(context, item, &number);
        JS_FreeValue(context, item);
        if (! ok) return std::nullopt;
        matrix(static_cast<int>(index / 4), static_cast<int>(index % 4)) = number;
    }
    return matrix;
}

std::optional<Eigen::Affine3f> GetBoneModelTransform(const WPSceneScriptHost::Opaque* opaque,
                                                     SceneNode* node, uint32_t bone_index) {
    const auto* puppet = AdvanceNodePuppetForScriptQuery(opaque, node);
    if (puppet == nullptr || bone_index >= puppet->bones.size()) return std::nullopt;
    return puppet->BoneModelTransform(bone_index);
}

std::optional<Eigen::Affine3f> GetBoneLocalTransform(const WPSceneScriptHost::Opaque* opaque,
                                                     SceneNode* node, uint32_t bone_index) {
    const auto* puppet = AdvanceNodePuppetForScriptQuery(opaque, node);
    if (puppet == nullptr || bone_index >= puppet->bones.size()) return std::nullopt;

    const auto& bone       = puppet->bones[bone_index];
    const auto& bone_model = puppet->BoneModelTransform(bone_index);
    if (bone.noAnimParent()) return bone_model;
    if (bone.anim_parent >= puppet->bones.size()) return std::nullopt;
    Eigen::Affine3f parent_model = puppet->BoneModelTransform(bone.anim_parent);
    if (puppet->world_anchored_bones) {
        parent_model = parent_model * puppet->bones[bone.anim_parent].inv_bind.matrix();
    }
    return parent_model.inverse() * bone_model;
}

bool SetBoneLocalTransform(const WPSceneScriptHost::Opaque* opaque, SceneNode* node,
                           uint32_t bone_index, const Eigen::Affine3f& transform) {
    auto* data = GetMutableNodeData(opaque, node);
    if (data == nullptr || ! data->puppet_layer.hasPuppet()) return false;
    return data->puppet_layer.SetLocalBoneTransform(bone_index, transform);
}

std::optional<Eigen::Matrix4d> GetAttachmentWorldTransform(const WPSceneScriptHost::Opaque* opaque,
                                                           SceneNode*                       node,
                                                           const WPPuppet::Attachment& attachment) {
    const auto bone_model = GetBoneModelTransform(opaque, node, attachment.bone_index);
    if (! bone_model.has_value()) return std::nullopt;

    node->UpdateTrans();
    return node->ModelTrans() * ((*bone_model) * attachment.transform).matrix().cast<double>();
}

template<typename Visitor>
void ForEachBaseLayerMaterial(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                              Visitor&& visitor) {
    if (opaque == nullptr || opaque->scene == nullptr) return;

    auto runtime_nodes_it = opaque->scene->objectRuntimeNodes.find(layer_id);
    if (runtime_nodes_it == opaque->scene->objectRuntimeNodes.end()) return;

    for (auto* node : runtime_nodes_it->second) {
        if (node == nullptr || node->Mesh() == nullptr || node->Mesh()->Material() == nullptr)
            continue;
        visitor(*node->Mesh()->Material(), node);
    }
}

template<typename Visitor>
void ForEachEffectLayerMaterial(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                                Visitor&& visitor) {
    if (opaque == nullptr || opaque->scene == nullptr) return;

    auto camera_names_it = opaque->scene->objectRuntimeCameraNames.find(layer_id);
    if (camera_names_it == opaque->scene->objectRuntimeCameraNames.end()) return;

    for (const auto& camera_name : camera_names_it->second) {
        auto camera_it = opaque->scene->cameras.find(camera_name);
        if (camera_it == opaque->scene->cameras.end() || ! camera_it->second->HasImgEffect())
            continue;

        auto* effect_layer = camera_it->second->GetImgEffect().get();
        if (effect_layer == nullptr) continue;

        for (size_t effect_index = 0; effect_index < effect_layer->EffectCount(); effect_index++) {
            auto& effect = effect_layer->GetEffect(effect_index);
            for (auto& effect_node : effect->nodes) {
                auto* node = effect_node.sceneNode.get();
                if (node == nullptr || node->Mesh() == nullptr ||
                    node->Mesh()->Material() == nullptr)
                    continue;
                visitor(*node->Mesh()->Material(), node, effect_node.output);
            }
        }
    }
}

const ShaderValue* FindMaterialUniformValue(const SceneMaterial& material,
                                            std::string_view     uniform_name);
std::optional<WPDynamicValue> DynamicValueFromShaderValue(const ShaderValue& value,
                                                          WPDynamicValue::Type hint);

struct EffectMaterialTarget {
    SceneImageEffect* effect { nullptr };
    SceneNode*        node { nullptr };
    SceneMaterial*    material { nullptr };
};

std::optional<EffectMaterialTarget>
FindEffectMaterialTarget(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                         uint32_t effect_index, uint32_t material_index) {
    if (opaque == nullptr || opaque->scene == nullptr) return std::nullopt;

    auto* effect = opaque->scene->FindImageEffect(layer_id, effect_index);
    if (effect == nullptr) return std::nullopt;

    uint32_t current_material_index = 0;
    for (auto& effect_node : effect->nodes) {
        auto* node = effect_node.sceneNode.get();
        if (node == nullptr || node->Mesh() == nullptr || node->Mesh()->Material() == nullptr)
            continue;

        // Wallpaper Engine exposes post-process material passes by their authored order inside the
        // effect. The render graph may carry additional output names, but script getMaterial(n)
        // needs to walk only the concrete shader materials so indexes such as godrays 0/1 resolve
        // to the downsample and cast passes that own the ray controls.
        if (current_material_index == material_index) {
            return EffectMaterialTarget {
                .effect   = effect,
                .node     = node,
                .material = node->Mesh()->Material(),
            };
        }
        current_material_index++;
    }

    return std::nullopt;
}

std::string ResolveRuntimeMaterialUniformName(const SceneMaterial& material,
                                              std::string_view     property_name) {
    const std::string authored_name(property_name);
    if (const auto alias_it = material.uniformAliases.find(authored_name);
        alias_it != material.uniformAliases.end()) {
        return alias_it->second;
    }

    for (const auto& [alias_name, uniform_name] : material.uniformAliases) {
        if (uniform_name == authored_name) return uniform_name;

        // Shader metadata commonly advertises `material:"raythreshold"` next to a GLSL uniform
        // named `g_Threshold`. This suffix fallback mirrors the parser-side user-property
        // resolver, so runtime scripts can use either the authored material name or the final
        // uniform name without hard-coding a project-specific alias table in the JS wrapper.
        if (uniform_name.size() > 2 && uniform_name.substr(2) == authored_name) {
            return uniform_name;
        }
        (void)alias_name;
    }

    return authored_name;
}

const ShaderValue* FindRuntimeMaterialUniformValue(const SceneMaterial& material,
                                                   std::string_view     property_name,
                                                   std::string* out_uniform_name = nullptr) {
    const std::string authored_name(property_name);
    std::string       uniform_name = ResolveRuntimeMaterialUniformName(material, authored_name);
    auto*             uniform      = FindMaterialUniformValue(material, uniform_name);
    if (uniform == nullptr && uniform_name != authored_name) {
        // If an authored alias points at a shader symbol that was optimized away or renamed, try
        // the original property name before reporting failure. This preserves direct uniform
        // writes for projects that already script GLSL names while still making alias misses
        // visible through the caller's diagnostics.
        uniform_name = authored_name;
        uniform      = FindMaterialUniformValue(material, uniform_name);
    }
    if (out_uniform_name != nullptr) *out_uniform_name = uniform_name;
    return uniform;
}

WPDynamicValue::Type RuntimeDynamicTypeForShaderValue(const ShaderValue& value) {
    // Runtime material setters do not have the parser's registration metadata, so infer the JS
    // conversion shape from the live uniform width. That keeps scalar godrays controls as numbers
    // and still supports Vec2/Vec3/Vec4 material uniforms for other effect scripts.
    switch (value.size()) {
        case 2:
            return WPDynamicValue::Type::Float2;
        case 3:
            return WPDynamicValue::Type::Float3;
        case 4:
            return WPDynamicValue::Type::Float4;
        case 1:
            return WPDynamicValue::Type::Float;
        default:
            return WPDynamicValue::Type::FloatVector;
    }
}

JSValue ShaderUniformValueToJS(JSContext* context, const ShaderValue& uniform) {
    const auto value =
        DynamicValueFromShaderValue(uniform, RuntimeDynamicTypeForShaderValue(uniform));
    if (! value.has_value()) return JS_UNDEFINED;

    const auto script_value = value->toScriptValue();
    return script_value.has_value() ? ScriptValueToJS(context, *script_value) : JS_UNDEFINED;
}

WPDynamicValue EvaluateRegistrationSetting(const WPSceneScriptRegistration& registration,
                                           const UserPropertyMap*           user_properties,
                                           const WPScriptEvaluationContext& base_context) {
    if (registration.property_name == "visible" && registration.setting.hasUserBinding() &&
        registration.setting.property.has_value()) {
        VisibleBinding binding;
        binding.value = true;
        registration.setting.value.tryGet(&binding.value);
        binding.user = *registration.setting.property;

        return WPDynamicValue(EvaluateVisibleBinding(binding, user_properties));
    }

    return registration.setting.evaluate(user_properties, nullptr, base_context);
}

void SyncEffectLayerTransforms(WPSceneScriptHost::Opaque* opaque, int32_t layer_id,
                               SceneNode* node) {
    if (opaque == nullptr || opaque->scene == nullptr || node == nullptr) return;

    auto camera_names_it = opaque->scene->objectRuntimeCameraNames.find(layer_id);
    if (camera_names_it == opaque->scene->objectRuntimeCameraNames.end()) return;

    for (const auto& camera_name : camera_names_it->second) {
        auto camera_it = opaque->scene->cameras.find(camera_name);
        if (camera_it == opaque->scene->cameras.end() || ! camera_it->second->HasImgEffect())
            continue;

        auto* effect_layer = camera_it->second->GetImgEffect().get();
        if (effect_layer == nullptr) continue;

        if (effect_layer->WorldNode() != nullptr) {
            effect_layer->SyncResolvedNodeToWorld();
        } else {
            effect_layer->SyncResolvedNodeToMatrix(
                Eigen::Affine3f(node->GetLocalTrans().cast<float>()));
        }
    }
}

const ShaderValue* FindMaterialUniformValue(const SceneMaterial& material,
                                            std::string_view     uniform_name) {
    const auto uniform_key = std::string(uniform_name);
    auto       const_it    = material.customShader.constValues.find(uniform_key);
    if (const_it != material.customShader.constValues.end())
        return std::addressof(const_it->second);

    if (material.customShader.shader == nullptr) return nullptr;
    auto default_it = material.customShader.shader->default_uniforms.find(uniform_key);
    if (default_it != material.customShader.shader->default_uniforms.end()) {
        return std::addressof(default_it->second);
    }
    return nullptr;
}

ShaderValue ShapeScalarShaderUniformValue(const SceneMaterial& material,
                                          std::string_view     uniform_name,
                                          const ShaderValue&   value) {
    if (value.size() != 1) return value;

    size_t target_size = 0;
    if (const auto* current = FindMaterialUniformValue(material, uniform_name); current != nullptr) {
        target_size = current->size();
    }
    if (target_size <= 1 || target_size > 4) return value;

    std::vector<float> broadcast(target_size, value[0]);
    return ShaderValue(std::span<const float>(broadcast));
}

bool MaterialHasUniform(const SceneMaterial& material, std::string_view uniform_name) {
    return FindMaterialUniformValue(material, uniform_name) != nullptr;
}

std::optional<float> ReadMaterialFloatUniform(const SceneMaterial& material,
                                              std::string_view     uniform_name) {
    const auto* value = FindMaterialUniformValue(material, uniform_name);
    if (value == nullptr || value->size() < 1) return std::nullopt;
    return (*value)[0];
}

std::optional<std::array<float, 3>> ReadMaterialFloat3Uniform(const SceneMaterial& material,
                                                              std::string_view     uniform_name) {
    const auto* value = FindMaterialUniformValue(material, uniform_name);
    if (value == nullptr || value->size() < 3) return std::nullopt;
    return std::array<float, 3> { (*value)[0], (*value)[1], (*value)[2] };
}

std::optional<std::array<float, 4>> ReadMaterialFloat4Uniform(const SceneMaterial& material,
                                                              std::string_view     uniform_name) {
    const auto* value = FindMaterialUniformValue(material, uniform_name);
    if (value == nullptr || value->size() < 4) return std::nullopt;
    return std::array<float, 4> { (*value)[0], (*value)[1], (*value)[2], (*value)[3] };
}

void WriteMaterialFloatUniform(SceneMaterial& material, std::string_view uniform_name,
                               float value) {
    material.customShader.constValues[std::string(uniform_name)] = ShaderValue(value);
}

void WriteMaterialFloat3Uniform(SceneMaterial& material, std::string_view uniform_name,
                                const std::array<float, 3>& value) {
    material.customShader.constValues[std::string(uniform_name)] = ShaderValue(value);
}

void WriteMaterialFloat4Uniform(SceneMaterial& material, std::string_view uniform_name,
                                const std::array<float, 4>& value) {
    material.customShader.constValues[std::string(uniform_name)] = ShaderValue(value);
}

std::optional<std::array<float, 3>> ReadMaterialLayerColor(const SceneMaterial& material) {
    if (const auto color = ReadMaterialFloat3Uniform(material, "g_Color"); color.has_value()) {
        return color;
    }
    if (const auto color4 = ReadMaterialFloat4Uniform(material, "g_Color4"); color4.has_value()) {
        return std::array<float, 3> { (*color4)[0], (*color4)[1], (*color4)[2] };
    }
    return std::nullopt;
}

bool ApplyMaterialLayerColor(SceneMaterial& material, const std::array<float, 3>& color) {
    if (! MaterialHasUniform(material, "g_Color") && ! MaterialHasUniform(material, "g_Color4")) {
        return false;
    }

    // Image and effect materials are inconsistent about whether RGB tint is exposed as g_Color or
    // packed into g_Color4. Update both when present so a live object color edit reaches the same
    // shader inputs that a cold parse initialized from the resolved user property.
    if (MaterialHasUniform(material, "g_Color")) {
        WriteMaterialFloat3Uniform(material, "g_Color", color);
    }
    if (MaterialHasUniform(material, "g_Color4")) {
        auto color4 = ReadMaterialFloat4Uniform(material, "g_Color4")
                          .value_or(std::array<float, 4> { color[0], color[1], color[2], 1.0f });
        color4[0]   = color[0];
        color4[1]   = color[1];
        color4[2]   = color[2];
        WriteMaterialFloat4Uniform(material, "g_Color4", color4);
    }
    return true;
}

std::optional<WPDynamicValue> DynamicValueFromShaderValue(const ShaderValue& value,
                                                          WPDynamicValue::Type hint) {
    return WPDynamicValue::FromUserPropertyValue(UserPropertyValue(value), hint);
}

std::optional<ShaderValue> ShaderValueFromDynamicValue(const WPDynamicValue& value) {
    if (std::array<float, 4> float4 {}; value.tryGet(&float4)) {
        return ShaderValue(float4);
    }
    if (std::array<float, 3> float3 {}; value.tryGet(&float3)) {
        return ShaderValue(float3);
    }
    if (std::array<float, 2> float2 {}; value.tryGet(&float2)) {
        return ShaderValue(float2);
    }
    if (std::vector<float> float_vector; value.tryGet(&float_vector)) {
        return ShaderValue(float_vector);
    }
    if (float float_value = 0.0f; value.tryGet(&float_value)) {
        return ShaderValue(float_value);
    }
    if (double double_value = 0.0; value.tryGet(&double_value)) {
        return ShaderValue(static_cast<float>(double_value));
    }
    if (int32_t int_value = 0; value.tryGet(&int_value)) {
        return ShaderValue(static_cast<float>(int_value));
    }
    if (uint32_t uint_value = 0; value.tryGet(&uint_value)) {
        return ShaderValue(static_cast<float>(uint_value));
    }
    if (bool bool_value = false; value.tryGet(&bool_value)) {
        return ShaderValue(bool_value ? 1.0f : 0.0f);
    }
    return std::nullopt;
}

SceneMaterial* RegistrationMaterial(const WPSceneScriptRegistration& registration) {
    if (registration.node == nullptr || registration.node->Mesh() == nullptr) return nullptr;
    return registration.node->Mesh()->Material();
}

const SceneMaterial* RegistrationMaterialConst(const WPSceneScriptRegistration& registration) {
    if (registration.node == nullptr || registration.node->Mesh() == nullptr) return nullptr;
    return registration.node->Mesh()->Material();
}

std::optional<WPDynamicValue>
ReadMaterialUniformPropertyValue(const WPSceneScriptRegistration& registration) {
    const auto* material = RegistrationMaterialConst(registration);
    if (material == nullptr) return std::nullopt;

    const auto* uniform = FindMaterialUniformValue(*material, registration.property_name);
    if (uniform == nullptr) return std::nullopt;

    return DynamicValueFromShaderValue(*uniform, registration.value_type);
}

bool ApplyMaterialUniformPropertyValue(WPSceneScriptHost::Opaque*       opaque,
                                       const WPSceneScriptRegistration& registration,
                                       const WPDynamicValue&            value) {
    auto* material = RegistrationMaterial(registration);
    if (material == nullptr) return false;

    const auto shader_value = ShaderValueFromDynamicValue(value);
    if (! shader_value.has_value()) {
        LOG_ERROR("SceneMaterialUniformApply: layer=%d uniform='%s' invalid value %s",
                  registration.object_id,
                  registration.property_name.c_str(),
                  value.describe().c_str());
        return false;
    }

    // CustomShaderPass writes material uniforms from SceneMaterial::constValues every frame.
    // Updating this map is therefore enough for live user-property colors to reach the next draw
    // without rebuilding the render graph or reloading the scene package.
    material->customShader.constValues[registration.property_name] =
        ShapeScalarShaderUniformValue(*material, registration.property_name, *shader_value);
    (void)opaque;
    return true;
}

bool IsFinalEffectCompositeNode(const SceneMaterial& material, const SceneNode* node,
                                std::string_view output) {
    if (output == SpecTex_Default) return true;
    if (node != nullptr && node->Camera().empty()) return true;

    const auto* shader = material.customShader.shader.get();
    if (shader == nullptr || shader->name != "genericimage3") return false;

    return std::find(material.textures.begin(),
                     material.textures.end(),
                     std::string(SpecTex_Default)) != material.textures.end();
}

bool IsCameraLinkedFromScene(const Scene& scene, std::string_view camera_name) {
    return std::any_of(
        scene.linkedCameras.begin(), scene.linkedCameras.end(), [camera_name](const auto& entry) {
            const auto& linked = entry.second;
            return std::find(linked.begin(), linked.end(), camera_name) != linked.end();
        });
}

void RefreshAlignedLayerPivot(SceneNode* node, std::string_view alignment,
                              const std::array<float, 2>& new_size) {
    if (node == nullptr) return;

    // Runtime `thisLayer.size` updates must follow the same contract as cold parsing: the authored
    // origin remains the script-visible pivot, and only the mesh-local alignment offset changes to
    // reflect the new quad dimensions.
    node->SetAlignmentOffset(ResolveImageAlignmentOffset(alignment, new_size));
}

bool UpdateQuadMeshSize(SceneMesh* mesh, const std::array<float, 2>& size) {
    if (mesh == nullptr || mesh->VertexCount() == 0) return false;

    auto& vertex = mesh->GetVertexArray(0);
    if (vertex.VertexCount() != 4) return false;

    const float      left     = -(size[0] * 0.5f);
    const float      right    = size[0] * 0.5f;
    const float      bottom   = -(size[1] * 0.5f);
    const float      top      = size[1] * 0.5f;
    const std::array position = {
        left, bottom, 0.0f, left, top, 0.0f, right, bottom, 0.0f, right, top, 0.0f,
    };

    if (! vertex.SetVertex(WE_IN_POSITION, position)) return false;
    mesh->SetDirty();
    return true;
}

Eigen::Matrix4d ResolveCursorHitModelTransform(const WPSceneScriptHost::Opaque* opaque,
                                               SceneNode*                       node,
                                               std::unordered_set<SceneNode*>&  resolving) {
    if (node == nullptr) return Eigen::Matrix4d::Identity();

    if (! resolving.insert(node).second) {
        // Cursor hit testing follows renderer-only transform bindings, which can reference
        // virtual parents that are not present in the SceneNode parent chain. A cycle would make
        // that virtual chain ambiguous, so log the exact node and fall back to the concrete scene
        // graph transform instead of guessing a partial parent order.
        LOG_ERROR("CursorHitTransform: recursive transform binding for layer=%d name='%s'",
                  node->ID(),
                  node->Name().c_str());
        node->UpdateTrans();
        return node->ModelTrans();
    }

    const auto*     node_data = GetNodeData(opaque, node);
    Eigen::Matrix4d resolved  = Eigen::Matrix4d::Identity();
    if (node_data != nullptr && node_data->InheritsSceneParentTransform() &&
        node_data->TransformParent() != nullptr) {
        // Effect-backed image layers often keep the script-facing authored node outside the real
        // SceneNode parent chain and express the render-time parent through WPShaderValueData.
        // Cursor hit bounds must use the same virtual parent transform as the renderer; otherwise
        // meshless authored layers are tested near their local origin while the visible composite
        // is drawn under its parent group.
        const Eigen::Matrix4d parent_model =
            ResolveCursorHitModelTransform(opaque, node_data->TransformParent(), resolving);
        resolved = parent_model * node->GetLocalTrans();
    } else {
        node->UpdateTrans();
        resolved = node->ModelTrans();
    }

    resolving.erase(node);
    return resolved;
}

Eigen::Matrix4d ResolveCursorHitModelTransform(const WPSceneScriptHost::Opaque* opaque,
                                               SceneNode*                       node) {
    std::unordered_set<SceneNode*> resolving;
    return ResolveCursorHitModelTransform(opaque, node, resolving);
}

std::optional<std::array<double, 4>> ComputeQuadBounds2D(const Eigen::Matrix4d&      model,
                                                         const std::array<float, 2>& size) {
    if (! std::isfinite(size[0]) || ! std::isfinite(size[1]) || size[0] <= 0.0f ||
        size[1] <= 0.0f) {
        return std::nullopt;
    }

    const double                         half_width  = static_cast<double>(size[0]) * 0.5;
    const double                         half_height = static_cast<double>(size[1]) * 0.5;
    const std::array<Eigen::Vector4d, 4> corners {
        Eigen::Vector4d(-half_width, -half_height, 0.0, 1.0),
        Eigen::Vector4d(-half_width, half_height, 0.0, 1.0),
        Eigen::Vector4d(half_width, -half_height, 0.0, 1.0),
        Eigen::Vector4d(half_width, half_height, 0.0, 1.0),
    };

    double min_x = std::numeric_limits<double>::max();
    double min_y = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double max_y = std::numeric_limits<double>::lowest();

    for (const auto& corner : corners) {
        const Eigen::Vector4d world = model * corner;
        min_x                       = std::min(min_x, world.x());
        min_y                       = std::min(min_y, world.y());
        max_x                       = std::max(max_x, world.x());
        max_y                       = std::max(max_y, world.y());
    }

    if (min_x > max_x || min_y > max_y) return std::nullopt;
    return std::array<double, 4> { min_x, min_y, max_x, max_y };
}

std::optional<std::array<double, 4>>
ComputeLayerQuadBounds2D(const WPSceneScriptHost::Opaque* opaque, SceneNode* node,
                         const std::array<float, 2>& size) {
    if (node == nullptr) return std::nullopt;

    // Meshless script targets still represent regular centered Wallpaper Engine quads. The key
    // difference from a renderable mesh is that their visible geometry lives in text primitives or
    // effect composite nodes, so this fallback reconstructs only the cursor bounds and leaves the
    // render graph untouched.
    const Eigen::Matrix4d model = ResolveCursorHitModelTransform(opaque, node);
    return ComputeQuadBounds2D(model, size);
}

std::optional<std::array<double, 4>> ComputeNodeBounds2D(const WPSceneScriptHost::Opaque* opaque,
                                                         SceneNode*                       node) {
    if (node == nullptr || node->Mesh() == nullptr || node->Mesh()->VertexCount() == 0)
        return std::nullopt;

    const auto& vertex_array = node->Mesh()->GetVertexArray(0);
    const auto  attributes   = vertex_array.GetAttrOffsetMap();
    const auto  position_it  = attributes.find(std::string(WE_IN_POSITION));
    if (position_it == attributes.end()) return std::nullopt;

    const usize position_offset = position_it->second.offset / sizeof(float);
    const usize stride          = vertex_array.OneSize();
    if (stride == 0 || position_offset + 2 >= stride) return std::nullopt;

    const float* data         = vertex_array.Data();
    const usize  vertex_count = vertex_array.VertexCount();
    if (data == nullptr || vertex_count == 0) return std::nullopt;

    // Even ordinary mesh bounds use the cursor-specific transform resolver so effect composites
    // and future runtime-only nodes do not diverge between their rendered model matrix and their
    // event target matrix.
    const Eigen::Matrix4d model = ResolveCursorHitModelTransform(opaque, node);

    double min_x = std::numeric_limits<double>::max();
    double min_y = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double max_y = std::numeric_limits<double>::lowest();

    for (usize index = 0; index < vertex_count; index++) {
        const float*          vertex = data + index * stride + position_offset;
        const Eigen::Vector4d local(vertex[0], vertex[1], vertex[2], 1.0);
        const Eigen::Vector4d world = model * local;
        min_x                       = std::min(min_x, world.x());
        min_y                       = std::min(min_y, world.y());
        max_x                       = std::max(max_x, world.x());
        max_y                       = std::max(max_y, world.y());
    }

    if (min_x > max_x || min_y > max_y) return std::nullopt;
    return std::array<double, 4> { min_x, min_y, max_x, max_y };
}

std::optional<std::array<double, 4>>
ComputeCursorTargetBounds2D(const WPSceneScriptHost::Opaque* opaque,
                            const WPSceneScriptRegistration& registration) {
    if (registration.node == nullptr) return std::nullopt;

    if (auto mesh_bounds = ComputeNodeBounds2D(opaque, registration.node);
        mesh_bounds.has_value()) {
        return mesh_bounds;
    }

    if (registration.target_kind != WPSceneScriptTargetKind::Layer) return std::nullopt;

    const int32_t layer_id = registration.object_id;
    if (const auto* image_layer = FindImageLayerById(opaque, layer_id); image_layer != nullptr) {
        // Effect-backed images can register scripts on a transform-only authored layer while the
        // visible pixels are rendered by source/final composite nodes. The image registry retains
        // the authored quad size, so combining it with ResolveCursorHitModelTransform recreates
        // the event target without adding any dummy mesh that could be picked up by rendering.
        return ComputeLayerQuadBounds2D(opaque, registration.node, image_layer->size);
    }

    if (const auto* text_layer = FindTextLayerById(opaque, layer_id); text_layer != nullptr) {
        // First-class text no longer needs a SceneMesh to render, which means mesh-only cursor
        // bounds would drop Clock-style hover scripts. Prefer the live primitive's visible box
        // when it exists and fall back to the authored text size for deferred logical layers.
        const std::array<float, 2> text_size = text_layer->primitive != nullptr
                                                   ? text_layer->primitive->VisibleDisplaySize()
                                                   : text_layer->object.size;
        return ComputeLayerQuadBounds2D(opaque, registration.node, text_size);
    }

    return std::nullopt;
}

bool InstanceReceivesCursor(const WPSceneScriptHost::Opaque* opaque, const ScriptInstance& instance,
                            const CursorPositionState& cursor) {
    if (! instance.initialized || instance.registration.node == nullptr) return false;
    if (instance.registration.target_kind != WPSceneScriptTargetKind::Layer) return false;

    const auto layer_id                    = instance.registration.object_id;
    const bool allow_hidden_visible_script = instance.registration.property_name == "visible";
    if (opaque != nullptr && opaque->scene != nullptr && ! allow_hidden_visible_script &&
        ! opaque->scene->IsLayerVisible(layer_id)) {
        return false;
    }

    const auto bounds = ComputeCursorTargetBounds2D(opaque, instance.registration);
    if (! bounds.has_value()) return false;
    return cursor.world_x >= (*bounds)[0] && cursor.world_x <= (*bounds)[2] &&
           cursor.world_y >= (*bounds)[1] && cursor.world_y <= (*bounds)[3];
}

JSValue MakeCursorEventObject(JSContext* context, const CursorPositionState& cursor,
                              bool left_down) {
    JSValue event  = JS_NewObject(context);
    // Cursor event positions must be real WE-style Vec instances rather than plain objects:
    // draggable scene scripts call event.worldPosition.add()/subtract() while computing offsets.
    JSValue world  = NumericVectorToJS(context, { cursor.world_x, cursor.world_y, 0.0 });
    JSValue screen = NumericVectorToJS(context, { cursor.screen_x, cursor.screen_y });

    JS_SetPropertyStr(context, event, "worldPosition", world);
    JS_SetPropertyStr(context, event, "screenPosition", screen);
    JS_SetPropertyStr(context, event, "leftDown", JS_NewBool(context, left_down));
    JS_SetPropertyStr(context, event, "button", JS_NewInt32(context, 0));
    return event;
}

bool CallScriptEvent(JSContext* context, JSValueConst callback, JSValueConst arg,
                     const char* stage) {
    if (JS_IsUndefined(callback)) return false;

    JSValue result = JS_Call(context, callback, JS_UNDEFINED, 1, &arg);
    if (JS_IsException(result)) {
        LogQuickJSException(context, stage);
        JS_FreeValue(context, result);
        return false;
    }
    JS_FreeValue(context, result);
    return true;
}

std::shared_ptr<Image> BuildMediaThumbnailImage(std::string_view key, int32_t width, int32_t height,
                                                std::span<const uint8_t> rgba) {
    if (width <= 0 || height <= 0 || rgba.empty()) {
        return CreateSceneScriptSolidImage(key, { 0, 0, 0, 0 });
    }

    return CreateSceneScriptRgbaImage(key, width, height, rgba);
}

void UpdateMediaTexture(WPSceneScriptHost::Opaque* opaque, std::string_view key, int32_t width,
                        int32_t height, std::span<const uint8_t> rgba) {
    if (opaque == nullptr || opaque->scene == nullptr) return;

    auto* synthetic_parser = AsSyntheticImageParser(opaque->scene->imageParser.get());
    if (synthetic_parser == nullptr) return;

    const auto image = BuildMediaThumbnailImage(key, width, height, rgba);
    if (image == nullptr) return;

    synthetic_parser->RegisterImage(std::string(key), image);
    opaque->scene->textures[std::string(key)] = SceneTexture {
        .url       = std::string(key),
        .sample    = image->header.sample,
        .format    = image->header.format,
        .isVideo   = false,
        .isSprite  = false,
        .width     = image->header.width,
        .height    = image->header.height,
        .mapWidth  = image->header.mapWidth,
        .mapHeight = image->header.mapHeight,
    };
    opaque->scene->dirtyImportedTextureKeys.insert(std::string(key));
}

void UpdateMediaThumbnailTexture(WPSceneScriptHost::Opaque*     opaque,
                                 const WPSceneScriptMediaState& media_state) {
    UpdateMediaTexture(opaque,
                       WP_SCENE_SCRIPT_MEDIA_THUMBNAIL_TEXTURE,
                       media_state.thumbnail_width,
                       media_state.thumbnail_height,
                       media_state.thumbnail_rgba);
    UpdateMediaTexture(opaque,
                       WP_SCENE_SCRIPT_MEDIA_PREVIOUS_THUMBNAIL_TEXTURE,
                       media_state.previous_thumbnail_width,
                       media_state.previous_thumbnail_height,
                       media_state.previous_thumbnail_rgba);
}

JSValue MakeMediaThumbnailEvent(JSContext* context, const WPSceneScriptMediaState& media_state) {
    JSValue event = JS_NewObject(context);
    JS_SetPropertyStr(
        context, event, "hasThumbnail", JS_NewBool(context, media_state.has_thumbnail));
    JS_SetPropertyStr(context,
                      event,
                      "primaryColor",
                      Vec3ToJS(context,
                               {
                                   static_cast<double>(media_state.primary_color[0]),
                                   static_cast<double>(media_state.primary_color[1]),
                                   static_cast<double>(media_state.primary_color[2]),
                               }));
    JS_SetPropertyStr(context,
                      event,
                      "secondaryColor",
                      Vec3ToJS(context,
                               {
                                   static_cast<double>(media_state.secondary_color[0]),
                                   static_cast<double>(media_state.secondary_color[1]),
                                   static_cast<double>(media_state.secondary_color[2]),
                               }));
    JS_SetPropertyStr(context,
                      event,
                      "tertiaryColor",
                      Vec3ToJS(context,
                               {
                                   static_cast<double>(media_state.tertiary_color[0]),
                                   static_cast<double>(media_state.tertiary_color[1]),
                                   static_cast<double>(media_state.tertiary_color[2]),
                               }));
    JS_SetPropertyStr(context,
                      event,
                      "textColor",
                      Vec3ToJS(context,
                               {
                                   static_cast<double>(media_state.text_color[0]),
                                   static_cast<double>(media_state.text_color[1]),
                                   static_cast<double>(media_state.text_color[2]),
                               }));
    JS_SetPropertyStr(context,
                      event,
                      "highContrastColor",
                      Vec3ToJS(context,
                               {
                                   static_cast<double>(media_state.high_contrast_color[0]),
                                   static_cast<double>(media_state.high_contrast_color[1]),
                                   static_cast<double>(media_state.high_contrast_color[2]),
                               }));
    return event;
}

JSValue MakeMediaPropertiesEvent(JSContext* context, const WPSceneScriptMediaState& media_state) {
    JSValue event = JS_NewObject(context);
    // Wallpaper Engine defines the extended media fields as optional, but scripts commonly compare
    // them with an empty string before falling back. Supplying empty strings instead of leaving
    // properties undefined keeps those fallback branches working with Linux MPRIS metadata.
    JS_SetPropertyStr(context, event, "title", JS_NewString(context, media_state.title.c_str()));
    JS_SetPropertyStr(context, event, "artist", JS_NewString(context, media_state.artist.c_str()));
    JS_SetPropertyStr(
        context, event, "albumTitle", JS_NewString(context, media_state.album_title.c_str()));
    JS_SetPropertyStr(
        context, event, "albumArtist", JS_NewString(context, media_state.album_artist.c_str()));
    JS_SetPropertyStr(
        context, event, "subTitle", JS_NewString(context, media_state.sub_title.c_str()));
    JS_SetPropertyStr(context, event, "genres", JS_NewString(context, media_state.genres.c_str()));
    JS_SetPropertyStr(
        context, event, "contentType", JS_NewString(context, media_state.content_type.c_str()));
    return event;
}

JSValue MakeMediaPlaybackEvent(JSContext* context, const WPSceneScriptMediaState& media_state) {
    JSValue event = JS_NewObject(context);
    JS_SetPropertyStr(context, event, "state", JS_NewInt32(context, media_state.playback_state));
    return event;
}

std::optional<WPDynamicValue> ReadDynamicValueFromJS(JSContext* context, JSValueConst value,
                                                     WPDynamicValue::Type hint) {
    const auto script_value = ScriptValueFromJS(context, value, hint);
    if (! script_value.has_value()) return std::nullopt;
    return WPDynamicValue::FromScriptValue(*script_value, hint);
}

constexpr float kSceneScriptDegreesToRadians = 0.017453292519943295769f;
constexpr float kSceneScriptRadiansToDegrees = 57.295779513082320877f;

bool IsAngleProperty(std::string_view property_name) { return property_name == "angles"; }

bool RegistrationUsesScriptAngleDegrees(const WPSceneScriptRegistration& registration) {
    // Wallpaper Engine stores layer/camera rotations in scene JSON as radians, but SceneScript
    // exposes the same "angles" properties as degrees. Keeping this predicate narrow prevents
    // material uniforms, animation playback values, and other Float3 properties from being
    // accidentally rescaled when they cross the script boundary.
    return IsAngleProperty(registration.property_name) &&
           (registration.target_kind == WPSceneScriptTargetKind::Layer ||
            registration.target_kind == WPSceneScriptTargetKind::Camera);
}

bool RegistrationUsesNumericVisibleScriptValue(const WPSceneScriptRegistration& registration) {
    // `visible` is stored as a renderer boolean, but Wallpaper Engine scene scripts are often
    // generated from scalar templates that expect the init/update argument to be a numeric value.
    // The 3666041758 ferrofluid layer is one concrete case: its visible script stores
    // `initialValue = typeof value === 'number' ? value : value.x`, so passing a JS boolean makes
    // `initialValue` undefined and the next update returns NaN. QuickJS then coerces NaN to false
    // for the boolean target and the renderer legitimately hides the whole layer. Keep the native
    // property type boolean, but expose visible registration values to scripts as 1.0/0.0 so both
    // scalar audio-response templates and normal truthy checks behave like Wallpaper Engine.
    if (registration.property_name != "visible") return false;
    return registration.target_kind == WPSceneScriptTargetKind::Layer ||
           registration.target_kind == WPSceneScriptTargetKind::Camera ||
           registration.target_kind == WPSceneScriptTargetKind::Effect ||
           registration.target_kind == WPSceneScriptTargetKind::AnimationLayer;
}

WPDynamicValue ConvertFloat3Scale(const WPDynamicValue& value, float scale) {
    std::array<float, 3> vector {};
    if (! value.tryGet(&vector)) return value;
    return WPDynamicValue(
        std::array<float, 3> { vector[0] * scale, vector[1] * scale, vector[2] * scale });
}

std::array<double, 3> ConvertEulerArrayScale(const std::array<double, 3>& value, double scale) {
    return { value[0] * scale, value[1] * scale, value[2] * scale };
}

WPDynamicValue ToScriptFacingRegistrationValue(const WPSceneScriptRegistration& registration,
                                               const WPDynamicValue&            value) {
    if (RegistrationUsesNumericVisibleScriptValue(registration)) {
        bool visible = false;
        if (value.tryGet(&visible)) return WPDynamicValue(visible ? 1.0f : 0.0f);
    }

    if (! RegistrationUsesScriptAngleDegrees(registration)) return value;

    // SceneScript authors write formulas such as atan2(...) * 180 / PI for layer angles. Convert
    // the renderer's internal radians to degrees before passing the update/init argument so those
    // formulas can preserve and modify the incoming value without mixing units.
    return ConvertFloat3Scale(value, kSceneScriptRadiansToDegrees);
}

WPDynamicValue FromScriptFacingRegistrationValue(const WPSceneScriptRegistration& registration,
                                                 const WPDynamicValue&            value) {
    if (! RegistrationUsesScriptAngleDegrees(registration)) return value;

    // Convert script-returned degrees back to the renderer's radian storage exactly at the
    // registration boundary. This keeps SceneNode, property animation, and JSON parsing code
    // single-purpose: they continue to deal only in renderer-native radians.
    return ConvertFloat3Scale(value, kSceneScriptDegreesToRadians);
}

WPDynamicValue ToScriptFacingLayerPropertyValue(std::string_view property_name,
                                                const WPDynamicValue& value) {
    if (! IsAngleProperty(property_name)) return value;

    // Direct script property access through thisLayer.angles follows the same degree-based
    // Wallpaper Engine contract as property update scripts, while SceneNode still stores radians.
    return ConvertFloat3Scale(value, kSceneScriptRadiansToDegrees);
}

WPDynamicValue FromScriptFacingLayerPropertyValue(std::string_view property_name,
                                                  const WPDynamicValue& value) {
    if (! IsAngleProperty(property_name)) return value;

    // Direct assignments like thisLayer.angles = new Vec3(...) arrive in degrees from scripts and
    // must be converted before they are compared with, or written into, the renderer node state.
    return ConvertFloat3Scale(value, kSceneScriptDegreesToRadians);
}

std::optional<WPDynamicValue> ReadLayerPropertyValue(const WPSceneScriptHost::Opaque* opaque,
                                                     SceneNode*                       node,
                                                     std::string_view property_name) {
    if (node == nullptr) return std::nullopt;

    if (property_name == "name") return WPDynamicValue(node->Name());
    if (property_name == "visible") {
        if (opaque != nullptr && opaque->scene != nullptr) {
            const auto layer_id = FindNodeId(opaque, node);
            if (layer_id != 0) {
                return WPDynamicValue(opaque->scene->GetLayerLocalVisibility(layer_id));
            }
        }
        return WPDynamicValue(node->Visible());
    }
    const auto text_layer_id = opaque != nullptr ? FindNodeId(opaque, node) : 0;
    const auto* text_layer =
        text_layer_id != 0 ? FindTextLayerById(opaque, text_layer_id) : nullptr;
    if (text_layer != nullptr && property_name == "origin") {
        // Text nodes keep their authored origin as the scene translation. Returning the registry value
        // rather than the node transform still matters for screen-anchored text, where the renderer may
        // temporarily shift the node into the active camera frame without changing script-visible state.
        return WPDynamicValue(text_layer->object.origin);
    }
    if (property_name == "origin") {
        const auto& value = node->Translate();
        return WPDynamicValue(std::array<float, 3> { value.x(), value.y(), value.z() });
    }
    if (property_name == "angles") {
        const auto& value = node->Rotation();
        return WPDynamicValue(std::array<float, 3> { value.x(), value.y(), value.z() });
    }
    if (property_name == "scale") {
        const auto& value = node->Scale();
        return WPDynamicValue(std::array<float, 3> { value.x(), value.y(), value.z() });
    }
    if (property_name == "parallaxDepth") {
        if (const auto* data = GetNodeData(opaque, node)) {
            return WPDynamicValue(data->parallaxDepth);
        }
    }
    if (opaque != nullptr) {
        const auto layer_id = FindNodeId(opaque, node);
        if (const auto* image_layer = FindImageLayerById(opaque, layer_id);
            image_layer != nullptr && property_name == "size") {
            return WPDynamicValue(image_layer->size);
        }
    }

    if (opaque != nullptr) {
        const auto layer_id = FindNodeId(opaque, node);
        if (layer_id != 0) {
            if (auto particle_value = ReadParticlePropertyValue(opaque, layer_id, property_name);
                particle_value.has_value()) {
                return particle_value;
            }

            std::optional<WPDynamicValue> result;
            bool                          inconsistent_color_copies = false;
            auto                          capture_color = [&](const SceneMaterial& material) {
                const auto color = ReadMaterialLayerColor(material);
                if (! color.has_value()) return;

                WPDynamicValue value(*color);
                if (! result.has_value()) {
                    result = value;
                    return;
                }
                if (! result->equals(value)) {
                    // Effect-backed image layers carry object tint in both their source material
                    // and their intermediate effect-pass materials. If an older live edit updated
                    // only one copy, returning "unknown" prevents the no-op fast path from hiding
                    // the stale material during the next user-property dispatch.
                    inconsistent_color_copies = true;
                }
            };
            ForEachBaseLayerMaterial(
                const_cast<WPSceneScriptHost::Opaque*>(opaque),
                layer_id,
                [&](const SceneMaterial& material, SceneNode*) {
                    if (property_name == "color") {
                        capture_color(material);
                        return;
                    }
                    if (property_name == "alpha") {
                        if (result.has_value()) return;
                        if (const auto alpha = ReadMaterialFloatUniform(material, "g_UserAlpha");
                            alpha.has_value()) {
                            result = WPDynamicValue(*alpha);
                        } else if (const auto alpha_fallback =
                                       ReadMaterialFloatUniform(material, "g_Alpha");
                                   alpha_fallback.has_value()) {
                            result = WPDynamicValue(*alpha_fallback);
                        } else if (const auto color4 =
                                       ReadMaterialFloat4Uniform(material, "g_Color4");
                                   color4.has_value()) {
                            result = WPDynamicValue((*color4)[3]);
                        }
                        return;
                    }
                    if (property_name == "brightness") {
                        if (const auto brightness =
                                ReadMaterialFloatUniform(material, "g_Brightness");
                            brightness.has_value()) {
                            result = WPDynamicValue(*brightness);
                        }
                    }
                });
            if (property_name == "color") {
                ForEachEffectLayerMaterial(
                    const_cast<WPSceneScriptHost::Opaque*>(opaque),
                    layer_id,
                    [&](const SceneMaterial& material,
                        SceneNode*           effect_node,
                        const std::string&   output) {
                        if (IsFinalEffectCompositeNode(material, effect_node, output)) return;
                        capture_color(material);
                    });
                if (inconsistent_color_copies) return std::nullopt;
            }
            if (result.has_value()) return result;
        }
    }
    return std::nullopt;
}

bool ApplyLayerPropertyValue(WPSceneScriptHost::Opaque* opaque, SceneNode* node,
                             std::string_view property_name, const WPDynamicValue& value) {
    if (node == nullptr) return false;

    if (property_name == "name") {
        std::string name;
        if (! value.tryGet(&name)) return false;
        node->SetName(name);
        if (opaque != nullptr && opaque->scene != nullptr) {
            opaque->scene->layerNameToId[name] = node->ID();
        }
        return true;
    }

    if (property_name == "visible") {
        bool visible = false;
        if (! value.tryGet(&visible)) return false;
        if (opaque != nullptr && opaque->scene != nullptr) {
            const auto layer_id = FindNodeId(opaque, node);
            if (layer_id != 0) {
                const bool was_effectively_visible = opaque->scene->IsLayerVisible(layer_id);
                opaque->scene->SetLayerLocalVisibility(layer_id, visible);
                if (visible && ! MaterializeDeferredVisibleLayerTreeIfNeeded(opaque, layer_id)) {
                    return false;
                }
                opaque->scene->ApplyLayerVisibility(layer_id);
                const bool is_effectively_visible = opaque->scene->IsLayerVisible(layer_id);
                if (was_effectively_visible != is_effectively_visible) {
                    // Visibility is now a render-graph residency boundary, not just a draw-time
                    // branch. Hidden layers are pruned from the graph so their pass-owned GPU
                    // resources can be released; visible layers must be reintroduced into the
                    // topology before their next frame can draw.
                    opaque->scene->MarkRenderGraphTopologyDirty();
                    if (!is_effectively_visible) {
                        QueueHiddenLayerTreeResourceRelease(opaque, layer_id);
                    } else {
                        CancelLayerTreeResourceRelease(opaque, layer_id);
                    }
                }
                return true;
            }
        }
        node->SetVisible(visible);
        return true;
    }

    if (opaque != nullptr) {
        const auto layer_id = FindNodeId(opaque, node);
        if (property_name == "size") {
            // `size` is ambiguous: image/text layers use a 2D quad size, while particle
            // instanceoverride.size is a scalar emitter multiplier. Try the particle override path
            // first when this node owns a particle subsystem, then fall back to image-layer resize.
            if (ApplyParticlePropertyValue(opaque, layer_id, property_name, value)) return true;

            auto* image_layer = FindImageLayerById(opaque, layer_id);
            if (image_layer == nullptr) return false;

            std::array<float, 2> new_size {};
            if (! value.tryGet(&new_size)) return false;
            new_size[0] = std::max(new_size[0], 1.0f);
            new_size[1] = std::max(new_size[1], 1.0f);

            const auto old_size = image_layer->size;
            if (std::abs(old_size[0] - new_size[0]) < 0.001f &&
                std::abs(old_size[1] - new_size[1]) < 0.001f) {
                return true;
            }

            bool updated_mesh = false;
            ForEachBaseLayerMaterial(opaque, layer_id, [&](SceneMaterial&, SceneNode* mesh_node) {
                if (mesh_node == nullptr || mesh_node->Mesh() == nullptr) return;
                updated_mesh = UpdateQuadMeshSize(mesh_node->Mesh(), new_size) || updated_mesh;
            });

            auto layer_node_it = opaque->scene->layerNodes.find(layer_id);
            if (layer_node_it != opaque->scene->layerNodes.end()) {
                RefreshAlignedLayerPivot(layer_node_it->second, image_layer->alignment, new_size);
            }

            if (auto camera_names_it = opaque->scene->objectRuntimeCameraNames.find(layer_id);
                camera_names_it != opaque->scene->objectRuntimeCameraNames.end()) {
                for (const auto& camera_name : camera_names_it->second) {
                    auto camera_it = opaque->scene->cameras.find(camera_name);
                    if (camera_it == opaque->scene->cameras.end()) continue;

                    if (! IsCameraLinkedFromScene(*opaque->scene, camera_name)) {
                        camera_it->second->SetWidth(
                            std::max(1.0, static_cast<double>(new_size[0])));
                        camera_it->second->SetHeight(
                            std::max(1.0, static_cast<double>(new_size[1])));
                        camera_it->second->Update();
                    }
                    if (camera_it->second->HasImgEffect()) {
                        auto& effect_layer = *camera_it->second->GetImgEffect();
                        updated_mesh =
                            UpdateQuadMeshSize(&effect_layer.FinalMesh(), new_size) || updated_mesh;
                        // Image-layer size edits also run through the resource-only rebuild path,
                        // which means the resolved effect output node keeps rendering until a full
                        // topology rebuild happens. Synchronizing the live output mesh here keeps
                        // runtime-resized effect quads visually consistent immediately.
                        effect_layer.SyncResolvedOutputMesh();
                    }
                }
            }

            if (auto render_targets_it = opaque->scene->objectRuntimeRenderTargets.find(layer_id);
                render_targets_it != opaque->scene->objectRuntimeRenderTargets.end() &&
                old_size[0] > 0.0f && old_size[1] > 0.0f) {
                const double scale_x =
                    static_cast<double>(new_size[0]) / static_cast<double>(old_size[0]);
                const double scale_y =
                    static_cast<double>(new_size[1]) / static_cast<double>(old_size[1]);
                for (const auto& render_target_name : render_targets_it->second) {
                    auto render_target_it = opaque->scene->renderTargets.find(render_target_name);
                    if (render_target_it == opaque->scene->renderTargets.end()) continue;
                    auto& render_target = render_target_it->second;
                    if (render_target.bind.enable) continue;
                    render_target.width = std::max(
                        1, static_cast<int32_t>(std::lround(render_target.width * scale_x)));
                    render_target.height = std::max(
                        1, static_cast<int32_t>(std::lround(render_target.height * scale_y)));
                    // Regular image-layer resizes scale both the backing image and the logical
                    // content rectangle together. Dedicated text bridges manage their own exact-
                    // size render-target updates through the text runtime path instead of sharing
                    // this generic image-layer resize contract.
                    render_target.mapWidth = std::max(
                        1,
                        static_cast<int32_t>(std::lround(render_target.ContentWidth() * scale_x)));
                    render_target.mapHeight = std::max(
                        1,
                        static_cast<int32_t>(std::lround(render_target.ContentHeight() * scale_y)));
                }
            }

            image_layer->size = new_size;
            // Image-layer size edits keep the same scene nodes and pass topology. Marking this as
            // a resource-only rebuild lets the renderer recreate effect targets/cameras without
            // discarding the static scene mesh uploads that are unrelated to the resize itself.
            opaque->scene->MarkRenderGraphResourcesDirty();
            return updated_mesh || opaque->scene->objectRuntimeCameraNames.count(layer_id) != 0;
        }
    }

    if (opaque != nullptr) {
        const auto layer_id = FindNodeId(opaque, node);
        if (layer_id != 0) {
            // Particle colors authored under instanceoverride are not material constants. Give the
            // particle runtime a chance to consume `colorn` and particle-layer `color` before the
            // generic image/text material tint path handles ordinary layer colors.
            if (ApplyParticlePropertyValue(opaque, layer_id, property_name, value)) return true;

            if (property_name == "color") {
                std::array<float, 3> color {};
                if (! value.tryGet(&color)) return false;

                bool        applied        = false;
                std::size_t base_targets   = 0;
                std::size_t effect_targets = 0;
                ForEachBaseLayerMaterial(
                    opaque, layer_id, [&](SceneMaterial& material, SceneNode*) {
                        if (! ApplyMaterialLayerColor(material, color)) return;
                        applied = true;
                        base_targets++;
                    });
                ForEachEffectLayerMaterial(
                    opaque,
                    layer_id,
                    [&](SceneMaterial& material, SceneNode* node, const std::string& output) {
                        if (IsFinalEffectCompositeNode(material, node, output)) return;
                        // Normal effect-pass materials inherit the layer color during cold parse.
                        // Keep their live constants synchronized with the source material, while
                        // leaving the final composite pass untinted because it intentionally
                        // samples the already colored effect output.
                        if (! ApplyMaterialLayerColor(material, color)) return;
                        applied = true;
                        effect_targets++;
                    });
                if (applied) {
                    LOG_VERBOSE("SceneLayerColorApply: layer=%d color=[%.3f, %.3f, %.3f] "
                                "base-targets=%zu effect-targets=%zu",
                                layer_id,
                                color[0],
                                color[1],
                                color[2],
                                base_targets,
                                effect_targets);
                }
                return applied;
            }

            if (property_name == "alpha") {
                float alpha = 0.0f;
                if (! value.tryGet(&alpha)) return false;

                bool applied     = false;
                auto apply_alpha = [&](SceneMaterial& material, bool update_user_alpha) {
                    bool updated = false;
                    if (MaterialHasUniform(material, "g_Alpha")) {
                        WriteMaterialFloatUniform(material, "g_Alpha", alpha);
                        updated = true;
                    }
                    if (update_user_alpha && MaterialHasUniform(material, "g_UserAlpha")) {
                        WriteMaterialFloatUniform(material, "g_UserAlpha", alpha);
                        updated = true;
                    }
                    if (MaterialHasUniform(material, "g_Color4")) {
                        auto color4 =
                            ReadMaterialFloat4Uniform(material, "g_Color4")
                                .value_or(std::array<float, 4> { 1.0f, 1.0f, 1.0f, alpha });
                        color4[3] = alpha;
                        WriteMaterialFloat4Uniform(material, "g_Color4", color4);
                        updated = true;
                    }
                    applied = applied || updated;
                };

                ForEachBaseLayerMaterial(
                    opaque, layer_id, [&](SceneMaterial& material, SceneNode*) {
                        apply_alpha(material, true);
                    });

                // Some effect chains keep the layer alpha on their final pass only.
                ForEachEffectLayerMaterial(
                    opaque,
                    layer_id,
                    [&](SceneMaterial& material, SceneNode* node, const std::string& output) {
                        const bool is_final_composite_node =
                            IsFinalEffectCompositeNode(material, node, output);
                        if (! is_final_composite_node &&
                            MaterialHasUniform(material, "g_UserAlpha")) {
                            return;
                        }

                        apply_alpha(material, is_final_composite_node);
                    });
                return applied;
            }

            if (property_name == "brightness") {
                float brightness = 0.0f;
                if (! value.tryGet(&brightness)) return false;

                bool applied = false;
                ForEachBaseLayerMaterial(
                    opaque, layer_id, [&](SceneMaterial& material, SceneNode*) {
                        if (! MaterialHasUniform(material, "g_Brightness")) return;
                        WriteMaterialFloatUniform(material, "g_Brightness", brightness);
                        applied = true;
                    });
                return applied;
            }
        }
    }

    std::array<float, 3> vector {};
    if (! value.tryGet(&vector)) return false;
    const auto layer_id = opaque != nullptr ? FindNodeId(opaque, node) : 0;
    auto apply_text_layer_transform = [&](std::string_view transform_name) -> std::optional<bool> {
        if (opaque == nullptr || opaque->scene == nullptr || layer_id == 0) return std::nullopt;
        if (FindTextLayerById(opaque, layer_id) == nullptr) return std::nullopt;

        const bool applied = ApplyTextLayerTransformValue(
            *opaque->scene, layer_id, node, transform_name, vector);
        if (applied) SyncEffectLayerTransforms(opaque, layer_id, node);
        return applied;
    };

    if (property_name == "origin") {
        if (auto text_result = apply_text_layer_transform(property_name);
            text_result.has_value()) return *text_result;

        node->SetTranslate(Eigen::Vector3f { vector[0], vector[1], vector[2] });
        if (opaque != nullptr) SyncEffectLayerTransforms(opaque, layer_id, node);
        return true;
    }
    if (property_name == "angles") {
        if (auto text_result = apply_text_layer_transform(property_name);
            text_result.has_value()) return *text_result;

        node->SetRotation(Eigen::Vector3f { vector[0], vector[1], vector[2] });
        if (opaque != nullptr) SyncEffectLayerTransforms(opaque, layer_id, node);
        return true;
    }
    if (property_name == "scale") {
        if (auto text_result = apply_text_layer_transform(property_name);
            text_result.has_value()) return *text_result;

        node->SetScale(Eigen::Vector3f { vector[0], vector[1], vector[2] });
        if (opaque != nullptr) SyncEffectLayerTransforms(opaque, layer_id, node);
        return true;
    }
    if (property_name == "parallaxDepth") {
        std::array<float, 2> parallax {};
        if (! value.tryGet(&parallax)) return false;
        if (opaque != nullptr && layer_id != 0) {
            if (auto* text_layer = FindTextLayerById(opaque, layer_id); text_layer != nullptr) {
                text_layer->object.parallaxDepth = parallax;
            }
        }
        if (auto* data = GetNodeData(opaque, node)) {
            data->parallaxDepth = parallax;
            return true;
        }
    }

    return false;
}

const Scene::CameraLayerRuntimeState*
FindCameraLayerState(const WPSceneScriptHost::Opaque* opaque, int32_t layer_id) {
    if (opaque == nullptr || opaque->scene == nullptr) return nullptr;
    auto it = opaque->scene->cameraLayers.find(layer_id);
    return it == opaque->scene->cameraLayers.end() ? nullptr : &it->second;
}

Scene::CameraLayerRuntimeState*
FindCameraLayerState(WPSceneScriptHost::Opaque* opaque, int32_t layer_id) {
    return const_cast<Scene::CameraLayerRuntimeState*>(
        FindCameraLayerState(static_cast<const WPSceneScriptHost::Opaque*>(opaque), layer_id));
}

std::optional<WPDynamicValue>
ReadCameraLayerPropertyValue(const WPSceneScriptHost::Opaque* opaque,
                             const WPSceneScriptRegistration& registration) {
    const auto* camera_layer = FindCameraLayerState(opaque, registration.object_id);
    if (camera_layer == nullptr || opaque == nullptr || opaque->scene == nullptr) {
        return std::nullopt;
    }

    if (registration.property_name == "visible") {
        return WPDynamicValue(opaque->scene->GetLayerLocalVisibility(registration.object_id));
    }
    if (registration.property_name == "origin") return WPDynamicValue(camera_layer->origin);
    if (registration.property_name == "angles") return WPDynamicValue(camera_layer->angles);
    if (registration.property_name == "zoom") {
        return WPDynamicValue(static_cast<float>(camera_layer->zoom));
    }
    if (registration.property_name == "fov") return WPDynamicValue(camera_layer->fov);

    return std::nullopt;
}

bool ApplyCameraLayerPropertyValue(WPSceneScriptHost::Opaque*       opaque,
                                   const WPSceneScriptRegistration& registration,
                                   const WPDynamicValue&            value) {
    auto* camera_layer = FindCameraLayerState(opaque, registration.object_id);
    if (camera_layer == nullptr || opaque == nullptr || opaque->scene == nullptr) return false;

    if (registration.property_name == "visible") {
        bool visible = false;
        if (! value.tryGet(&visible)) return false;
        // Camera visibility is still authored as layer visibility, but changing it must also
        // reselect the active camera immediately because Wallpaper Engine falls back to the next
        // visible camera layer as soon as this property changes.
        opaque->scene->SetLayerLocalVisibility(registration.object_id, visible);
        opaque->scene->ApplyLayerVisibility(registration.object_id);
        return true;
    }

    if (registration.property_name == "origin") {
        std::array<float, 3> origin {};
        if (! value.tryGet(&origin)) return false;
        camera_layer->origin = origin;
        if (camera_layer->node) {
            camera_layer->node->SetTranslate(
                opaque->scene->ResolveCameraLayerNodeTranslation(origin));
        }
        opaque->scene->UpdateActiveCameraLayer();
        return true;
    }

    if (registration.property_name == "angles") {
        std::array<float, 3> angles {};
        if (! value.tryGet(&angles)) return false;
        camera_layer->angles = angles;
        if (camera_layer->node) {
            camera_layer->node->SetRotation(Eigen::Vector3f { angles[0], angles[1], angles[2] });
        }
        opaque->scene->UpdateActiveCameraLayer();
        return true;
    }

    float scalar = 0.0f;
    if (! value.tryGet(&scalar) || !std::isfinite(scalar)) return false;
    if (registration.property_name == "zoom") {
        // Store the authored zoom even when the layer is inactive. UpdateActiveCameraLayer() will
        // apply it only if this camera layer currently owns the view, matching WE's visibility
        // driven camera selection without throwing away offscreen camera animation state.
        camera_layer->zoom = scalar;
        opaque->scene->UpdateActiveCameraLayer();
        return true;
    }
    if (registration.property_name == "fov") {
        camera_layer->fov = scalar;
        opaque->scene->UpdateActiveCameraLayer();
        return true;
    }

    return false;
}

std::optional<WPDynamicValue> ReadScenePropertyValue(const WPSceneScriptHost::Opaque* opaque,
                                                     std::string_view property_name) {
    if (opaque == nullptr || opaque->scene == nullptr) return std::nullopt;

    if (property_name == "clearcolor") return WPDynamicValue(opaque->scene->clearColor);
    if (property_name == "ambientcolor") return WPDynamicValue(opaque->scene->ambientColor);
    if (property_name == "skylightcolor") return WPDynamicValue(opaque->scene->skylightColor);
    if (property_name == "bloom") return WPDynamicValue(opaque->scene->bloom.enabled);
    if (property_name == "bloomstrength") return WPDynamicValue(opaque->scene->bloom.strength);
    if (property_name == "bloomthreshold") return WPDynamicValue(opaque->scene->bloom.threshold);
    if (property_name == "bloomtint") return WPDynamicValue(opaque->scene->bloom.tint);
    if (property_name == "cameraparallax") return WPDynamicValue(opaque->scene->cameraParallax);
    if (property_name == "cameraparallaxamount") {
        return WPDynamicValue(opaque->scene->cameraParallaxAmount);
    }
    if (property_name == "cameraparallaxdelay") {
        return WPDynamicValue(opaque->scene->cameraParallaxDelay);
    }
    if (property_name == "cameraparallaxmouseinfluence") {
        return WPDynamicValue(opaque->scene->cameraParallaxMouseInfluence);
    }
    if (const auto* camera = GetPerspectiveSceneCamera(opaque)) {
        if (property_name == "fov") return WPDynamicValue(static_cast<float>(camera->Fov()));
        if (property_name == "nearz") return WPDynamicValue(static_cast<float>(camera->NearClip()));
        if (property_name == "farz") return WPDynamicValue(static_cast<float>(camera->FarClip()));
    }
    return std::nullopt;
}

bool ApplyScenePropertyValue(WPSceneScriptHost::Opaque* opaque, std::string_view property_name,
                             const WPDynamicValue& value) {
    if (opaque == nullptr || opaque->scene == nullptr) return false;

    const auto update_scene_bloom_uniform =
        [&](std::string_view uniform_name, const ShaderValue& shader_value) -> bool {
        if (opaque->scene->bloom.node == nullptr || opaque->scene->bloom.node->Mesh() == nullptr ||
            opaque->scene->bloom.node->Mesh()->Material() == nullptr) {
            return false;
        }
        // Scene Bloom's runtime-editable uniforms live on the first extraction pass. Later blur and
        // combine passes intentionally stay parameter-free, matching Wallpaper Engine's utility
        // materials while avoiding render-graph rebuilds for user-property changes.
        opaque->scene->bloom.node->Mesh()->Material()->customShader.constValues
            [std::string(uniform_name)] = shader_value;
        return true;
    };

    if (property_name == "clearcolor") {
        std::array<float, 3> clear_color {};
        if (! value.tryGet(&clear_color)) return false;
        opaque->scene->clearColor = clear_color;
        return true;
    }
    if (property_name == "ambientcolor") {
        std::array<float, 3> ambient_color {};
        if (! value.tryGet(&ambient_color)) return false;
        opaque->scene->ambientColor = ambient_color;
        UpdateSceneLightingUniforms(opaque);
        return true;
    }
    if (property_name == "skylightcolor") {
        std::array<float, 3> skylight_color {};
        if (! value.tryGet(&skylight_color)) return false;
        opaque->scene->skylightColor = skylight_color;
        UpdateSceneLightingUniforms(opaque);
        return true;
    }
    if (property_name == "bloom") {
        bool enabled = false;
        if (! value.tryGet(&enabled)) return false;
        opaque->scene->bloom.enabled = enabled;
        // Scene-level Bloom follows the same stable-topology contract as authored image effects:
        // the pass stays in the render graph and the user toggle only changes a uniform. Rebuilding
        // the graph here would recreate Vulkan passes/framebuffers and make the highlight switch
        // noticeably heavier than ordinary post-process visibility changes.
        if (!update_scene_bloom_uniform("u_enabled", ShaderValue(enabled ? 1.0f : 0.0f))) {
            LOG_INFO("SceneGeneralApply: property='bloom' value=%s bloom-node=missing",
                     enabled ? "true" : "false");
            return true;
        }
        LOG_INFO("SceneGeneralApply: property='bloom' value=%s topology-dirty=false",
                 enabled ? "true" : "false");
        return true;
    }
    if (property_name == "bloomtint") {
        std::array<float, 3> tint {};
        if (! value.tryGet(&tint)) return false;
        opaque->scene->bloom.tint = tint;
        // Write both the stock Wallpaper Engine uniform name and the older Hanabi alias so already
        // parsed legacy single-pass Bloom nodes can still react during a runtime transition.
        update_scene_bloom_uniform("g_BloomTint", ShaderValue(tint));
        update_scene_bloom_uniform("u_tint", ShaderValue(tint));
        LOG_INFO("SceneGeneralApply: property='bloomtint' value=[%.3f, %.3f, %.3f]",
                 tint[0],
                 tint[1],
                 tint[2]);
        return true;
    }
    if (property_name == "cameraparallax") {
        bool enabled = false;
        if (! value.tryGet(&enabled)) return false;
        opaque->scene->cameraParallax = enabled;
        ApplySceneCameraParallax(opaque);
        LOG_INFO("SceneGeneralApply: property='cameraparallax' value=%s",
                 enabled ? "true" : "false");
        return true;
    }

    float scalar = 0.0f;
    if (! value.tryGet(&scalar)) return false;
    if (property_name == "cameraparallaxamount") {
        opaque->scene->cameraParallaxAmount = scalar;
        ApplySceneCameraParallax(opaque);
        LOG_INFO("SceneGeneralApply: property='cameraparallaxamount' value=%.3f", scalar);
        return true;
    }
    if (property_name == "bloomstrength") {
        opaque->scene->bloom.strength = scalar;
        // The exact WE-compatible chain consumes `g_BloomStrength`; `u_strength` remains for
        // compatibility with any in-memory legacy parser output created before this graph was built.
        update_scene_bloom_uniform("g_BloomStrength", ShaderValue(scalar));
        update_scene_bloom_uniform("u_strength", ShaderValue(scalar));
        LOG_INFO("SceneGeneralApply: property='bloomstrength' value=%.3f", scalar);
        return true;
    }
    if (property_name == "bloomthreshold") {
        opaque->scene->bloom.threshold = scalar;
        // The downsample/extract shader thresholds on the maximum RGB channel exactly like WE's
        // stock shader, so runtime property updates must target the stock uniform name.
        update_scene_bloom_uniform("g_BloomThreshold", ShaderValue(scalar));
        update_scene_bloom_uniform("u_threshold", ShaderValue(scalar));
        LOG_INFO("SceneGeneralApply: property='bloomthreshold' value=%.3f", scalar);
        return true;
    }
    if (property_name == "cameraparallaxdelay") {
        opaque->scene->cameraParallaxDelay = scalar;
        ApplySceneCameraParallax(opaque);
        LOG_INFO("SceneGeneralApply: property='cameraparallaxdelay' value=%.3f", scalar);
        return true;
    }
    if (property_name == "cameraparallaxmouseinfluence") {
        opaque->scene->cameraParallaxMouseInfluence = scalar;
        ApplySceneCameraParallax(opaque);
        LOG_INFO("SceneGeneralApply: property='cameraparallaxmouseinfluence' value=%.3f", scalar);
        return true;
    }

    auto* camera = GetPerspectiveSceneCamera(opaque);
    if (camera == nullptr) return false;
    if (property_name == "fov") {
        camera->SetFov(scalar);
        camera->Update();
        return true;
    }
    if (property_name == "nearz") {
        camera->SetNearClip(scalar);
        camera->Update();
        return true;
    }
    if (property_name == "farz") {
        camera->SetFarClip(scalar);
        camera->Update();
        return true;
    }
    return false;
}

std::optional<WPDynamicValue>
ReadAnimationLayerPropertyValue(const WPSceneScriptHost::Opaque* opaque, SceneNode* node,
                                usize index, std::string_view property_name) {
    const auto read_from_layer =
        [&](const WPPuppetLayer::AnimationLayer* layer) -> std::optional<WPDynamicValue> {
        if (layer == nullptr) return std::nullopt;

        if (property_name == "visible") return WPDynamicValue(layer->visible);
        if (property_name == "rate") return WPDynamicValue(static_cast<float>(layer->rate));
        if (property_name == "blend") return WPDynamicValue(static_cast<float>(layer->blend));
        return std::nullopt;
    };

    const auto target_nodes = CollectPuppetLayerNodes(opaque, node);
    if (target_nodes.empty()) {
        const auto* layer = FindAnimationLayerState(opaque, node, index);
        return read_from_layer(layer);
    }

    std::optional<WPDynamicValue> result;
    for (auto* target_node : target_nodes) {
        const auto* data = GetNodeData(opaque, target_node);
        const auto* layer =
            data != nullptr ? data->puppet_layer.AnimationLayerState(index) : nullptr;
        const auto value = read_from_layer(layer);
        if (! value.has_value()) continue;
        if (! result.has_value()) {
            result = value;
            continue;
        }
        if (! result->equals(*value)) {
            // Duplicated effect-backed puppet nodes can temporarily diverge after a runtime edit
            // that touched only the logical node. Reporting "unknown" here disables the usual
            // no-op fast path, forcing the next user-property application to resynchronize every
            // rendered puppet copy instead of trusting the first matching state it finds.
            return std::nullopt;
        }
    }

    return result;
}

bool ApplyAnimationLayerPropertyValue(WPSceneScriptHost::Opaque* opaque, SceneNode* node,
                                      usize index, std::string_view property_name,
                                      const WPDynamicValue& value) {
    const auto target_nodes = CollectPuppetLayerNodes(opaque, node);
    if (target_nodes.empty()) return false;

    int32_t logical_layer_id = 0;
    if (opaque != nullptr) {
        logical_layer_id = FindOwningLayerId(opaque, node);
        if (logical_layer_id == 0) logical_layer_id = FindNodeId(opaque, node);
    }

    bool  applied        = false;
    bool  logged_visible = false;
    bool  logged_blend   = false;
    bool  visible_value  = false;
    float rate_value     = 0.0f;
    float blend_value    = 0.0f;

    if (property_name == "visible" && ! value.tryGet(&visible_value)) return false;
    if (property_name == "rate" && ! value.tryGet(&rate_value)) return false;
    if (property_name == "blend" && ! value.tryGet(&blend_value)) return false;

    for (auto* target_node : target_nodes) {
        auto* data = GetNodeData(opaque, target_node);
        if (data == nullptr || ! data->puppet_layer.hasPuppet()) continue;

        auto* layer = data->puppet_layer.AnimationLayerState(index);
        if (layer == nullptr) continue;

        if (property_name == "visible") {
            layer->visible = visible_value;
            // Animation-layer visibility changes alter the normalized puppet blend stack. Recompute
            // every duplicate puppet copy immediately so an effect-backed layer uses the same base
            // pose fallback as a cold parse with this user property already applied.
            data->puppet_layer.RefreshBlendState();
            applied        = true;
            logged_visible = true;
            continue;
        }
        if (property_name == "rate") {
            layer->rate = rate_value;
            applied     = true;
            continue;
        }
        if (property_name == "blend") {
            layer->blend = blend_value;
            // Blend edits use the same runtime path as visibility edits: the stored per-layer
            // weight and base-pose fallback are derived values, so each rendered puppet copy must
            // rebuild them after the authored blend amount changes.
            data->puppet_layer.RefreshBlendState();
            applied      = true;
            logged_blend = true;
            continue;
        }

        return false;
    }

    if (! applied) return false;

    if (logged_visible || logged_blend) {
        const auto state       = ReadAnimationLayerPropertyValue(opaque, node, index, "visible");
        const auto blend_state = ReadAnimationLayerPropertyValue(opaque, node, index, "blend");
        bool       visible     = visible_value;
        float      blend       = blend_value;
        if (state.has_value()) state->tryGet(&visible);
        if (blend_state.has_value()) blend_state->tryGet(&blend);
        LOG_INFO("SceneAnimationLayerApply: layer=%d animation-index=%zu visible=%s blend=%.3f "
                 "blend-refreshed=true targets=%zu",
                 logical_layer_id,
                 static_cast<size_t>(index),
                 visible ? "true" : "false",
                 blend,
                 target_nodes.size());
    }

    return true;
}

const SceneImageEffect* FindEffectTarget(const WPSceneScriptHost::Opaque* opaque,
                                         const WPSceneScriptRegistration& registration) {
    if (opaque == nullptr || opaque->scene == nullptr) return nullptr;
    if (registration.target_id != 0) {
        return opaque->scene->FindImageEffectById(registration.object_id, registration.target_id);
    }
    return opaque->scene->FindImageEffect(registration.object_id, registration.target_index);
}

SceneImageEffect* FindEffectTarget(WPSceneScriptHost::Opaque*       opaque,
                                   const WPSceneScriptRegistration& registration) {
    if (opaque == nullptr || opaque->scene == nullptr) return nullptr;
    if (registration.target_id != 0) {
        return opaque->scene->FindImageEffectById(registration.object_id, registration.target_id);
    }
    return opaque->scene->FindImageEffect(registration.object_id, registration.target_index);
}

std::optional<WPDynamicValue>
ReadEffectPropertyValue(const WPSceneScriptHost::Opaque* opaque,
                        const WPSceneScriptRegistration& registration) {
    const auto* effect = FindEffectTarget(opaque, registration);
    if (effect == nullptr) return std::nullopt;

    if (registration.property_name == "visible") return WPDynamicValue(effect->LocalVisible());
    return std::nullopt;
}

bool ApplyEffectPropertyValue(WPSceneScriptHost::Opaque*       opaque,
                              const WPSceneScriptRegistration& registration,
                              const WPDynamicValue&            value) {
    if (opaque == nullptr || opaque->scene == nullptr) return false;
    if (registration.property_name != "visible") return false;

    bool visible = false;
    if (! value.tryGet(&visible)) return false;
    const bool applied = registration.target_id != 0
                             ? opaque->scene->SetEffectLocalVisibilityById(
                                   registration.object_id, registration.target_id, visible)
                             : opaque->scene->SetEffectLocalVisibility(
                                   registration.object_id, registration.target_index, visible);
    if (applied) {
        LOG_INFO("SceneVisibilityEffectApply: layer=%d effect-id=%d effect-index=%u name='%s' "
                 "visible=%s topology-dirty=%s",
                 registration.object_id,
                 registration.target_id,
                 registration.target_index,
                 registration.object_name.c_str(),
                 visible ? "true" : "false",
                 opaque->scene->renderGraphTopologyDirty ? "true" : "false");
    }
    return applied;
}

LayerValueHint RegistrationValueType(const WPSceneScriptRegistration& registration) {
    if (registration.target_kind == WPSceneScriptTargetKind::Scene) {
        return SceneValueType(registration.property_name);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Sound) {
        return SoundValueType(registration.property_name);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Camera) {
        return CameraLayerValueType(registration.property_name);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::AnimationLayer) {
        return AnimationLayerValueType(registration.property_name);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Effect) {
        return EffectValueType(registration.property_name);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::MaterialUniform) {
        return { registration.value_type, true };
    }
    return LayerValueType(registration.property_name);
}

std::optional<WPDynamicValue> ReadRegistrationValue(const WPSceneScriptHost::Opaque* opaque,
                                                    const WPSceneScriptRegistration& registration) {
    if (registration.target_kind == WPSceneScriptTargetKind::Scene) {
        return ReadScenePropertyValue(opaque, registration.property_name);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Sound) {
        return ReadSoundPropertyValue(opaque, registration.object_id, registration.property_name);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Camera) {
        return ReadCameraLayerPropertyValue(opaque, registration);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Layer) {
        if (const auto text_value = ReadTextLayerPropertyValue(
                opaque, registration.object_id, registration.property_name);
            text_value.has_value()) {
            return text_value;
        }
    }
    if (registration.target_kind == WPSceneScriptTargetKind::AnimationLayer) {
        return ReadAnimationLayerPropertyValue(
            opaque, registration.node, registration.target_index, registration.property_name);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Effect) {
        return ReadEffectPropertyValue(opaque, registration);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::MaterialUniform) {
        return ReadMaterialUniformPropertyValue(registration);
    }
    return ReadLayerPropertyValue(opaque, registration.node, registration.property_name);
}

bool ApplyRegistrationValue(WPSceneScriptHost::Opaque*       opaque,
                            const WPSceneScriptRegistration& registration,
                            const WPDynamicValue&            value) {
    const auto runtime_value = ClampOpacityRegistrationValue(registration, value);
    const auto current = ReadRegistrationValue(opaque, registration);
    const bool is_text_registration =
        registration.target_kind == WPSceneScriptTargetKind::Layer &&
        FindTextLayerById(opaque, registration.object_id) != nullptr &&
        HasTextLayerProperty(registration.property_name);
    if (current.has_value() && current->equals(runtime_value)) {
        // Most scene scripts return the current value every frame even when the visual state is
        // unchanged. Treat that as a no-op without logging so steady-state text scripts behave like
        // cheap state polling instead of forcing the logger onto the critical render/update path.
        return true;
    }

    if (is_text_registration) {
        return ApplyTextLayerPropertyValue(
            opaque, registration.object_id, registration.property_name, runtime_value);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::AnimationLayer) {
        return ApplyAnimationLayerPropertyValue(opaque,
                                                registration.node,
                                                registration.target_index,
                                                registration.property_name,
                                                runtime_value);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Effect) {
        return ApplyEffectPropertyValue(opaque, registration, runtime_value);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Scene) {
        return ApplyScenePropertyValue(opaque, registration.property_name, runtime_value);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Sound) {
        return ApplySoundPropertyValue(
            opaque, registration.object_id, registration.property_name, runtime_value);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::Camera) {
        return ApplyCameraLayerPropertyValue(opaque, registration, runtime_value);
    }
    if (registration.target_kind == WPSceneScriptTargetKind::MaterialUniform) {
        return ApplyMaterialUniformPropertyValue(opaque, registration, runtime_value);
    }
    return ApplyLayerPropertyValue(
        opaque, registration.node, registration.property_name, runtime_value);
}

void FreeJSValue(JSContext* context, JSValue& value) {
    if (! JS_IsUndefined(value)) {
        JS_FreeValue(context, value);
        value = JS_UNDEFINED;
    }
}

JSValue NativeConsoleLog(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

JSValue NativeConsoleWarn(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    std::string message;
    for (int i = 0; i < argc; i++) {
        if (i != 0) message.push_back(' ');
        const char* text = JS_ToCString(context, argv[i]);
        if (text != nullptr) {
            message += text;
            JS_FreeCString(context, text);
        }
    }
    LOG_ERROR("SceneScript warn: %s", message.c_str());
    return JS_UNDEFINED;
}

JSValue NativeGetLayerProperty(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_UNDEFINED;

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_UNDEFINED;

    std::string property_name;
    if (! ReadJSString(context, argv[1], &property_name)) return JS_UNDEFINED;

    if (const auto original_value = ReadOriginalLayerPropertyValue(opaque, node_id, property_name);
        original_value.has_value()) {
        // originalOrigin must be read before the generic layer fallback because it is a virtual
        // Wallpaper Engine API member backed by the initial authored layer JSON, not by mutable
        // SceneNode transform fields.
        const auto script_value = original_value->toScriptValue();
        return script_value.has_value() ? ScriptValueToJS(context, *script_value) : JS_UNDEFINED;
    }

    if (const auto sound_handle = FindSoundHandleByLayerId(opaque, node_id);
        sound_handle.has_value()) {
        if (property_name == "name") {
            auto it = opaque->scene->layerNameToId.begin();
            for (; it != opaque->scene->layerNameToId.end(); ++it) {
                if (it->second == node_id) break;
            }
            return it == opaque->scene->layerNameToId.end()
                       ? JS_UNDEFINED
                       : JS_NewStringLen(context, it->first.c_str(), it->first.size());
        }
        if (property_name == "volume") {
            return JS_NewFloat64(context, opaque->scene->soundManager->StreamVolume(*sound_handle));
        }
        return JS_UNDEFINED;
    }

    if (const auto text_value = ReadTextLayerPropertyValue(opaque, node_id, property_name);
        text_value.has_value()) {
        const auto script_value =
            ToScriptFacingLayerPropertyValue(property_name, *text_value).toScriptValue();
        return script_value.has_value() ? ScriptValueToJS(context, *script_value) : JS_UNDEFINED;
    }

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr) return JS_UNDEFINED;

    const auto value = ReadLayerPropertyValue(opaque, node, property_name);
    if (! value.has_value()) return JS_UNDEFINED;
    const auto script_value = ToScriptFacingLayerPropertyValue(property_name, *value).toScriptValue();
    return script_value.has_value() ? ScriptValueToJS(context, *script_value) : JS_UNDEFINED;
}

JSValue NativeSetLayerProperty(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 3) return JS_FALSE;

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_FALSE;

    std::string property_name;
    if (! ReadJSString(context, argv[1], &property_name)) return JS_FALSE;

    if (const auto sound_handle = FindSoundHandleByLayerId(opaque, node_id);
        sound_handle.has_value()) {
        if (property_name == "volume") {
            double volume = 0.0;
            if (JS_ToFloat64(context, &volume, argv[2]) != 0) return JS_FALSE;
            return JS_NewBool(context,
                              opaque->scene->soundManager->SetStreamVolume(
                                  *sound_handle, static_cast<float>(volume)));
        }
        return JS_FALSE;
    }

    if (FindTextLayerById(opaque, node_id) != nullptr && HasTextLayerProperty(property_name)) {
        const auto hint = LayerValueType(property_name);
        if (! hint.supported) return JS_FALSE;
        const auto value = ReadDynamicValueFromJS(context, argv[2], hint.type);
        if (! value.has_value()) return JS_FALSE;
        const auto runtime_value = FromScriptFacingLayerPropertyValue(property_name, *value);
        if (const auto current = ReadTextLayerPropertyValue(opaque, node_id, property_name);
            current.has_value() && current->equals(runtime_value)) {
            // Per-frame authored scripts frequently assign the same visible/origin/text value back
            // to a layer. Treat identical writes as successful no-ops so those scripts do not force
            // transform, material, or bridge-resource work when the visual state is already
            // current.
            return JS_TRUE;
        }
        return JS_NewBool(context,
                          ApplyTextLayerPropertyValue(
                              opaque, node_id, property_name, runtime_value));
    }

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr) return JS_FALSE;

    const auto hint = LayerValueType(property_name);
    if (! hint.supported) return JS_FALSE;

    const auto value = ReadDynamicValueFromJS(context, argv[2], hint.type);
    if (! value.has_value()) return JS_FALSE;
    const auto runtime_value = FromScriptFacingLayerPropertyValue(property_name, *value);
    if (const auto current = ReadLayerPropertyValue(opaque, node, property_name);
        current.has_value() && current->equals(runtime_value)) {
        // Scene.on('update') scripts often drive layer visibility by assigning true/false every
        // frame. Returning early here preserves the Wallpaper Engine script contract while keeping
        // unchanged layers off the render-graph dirtiness path.
        return JS_TRUE;
    }

    if (opaque != nullptr && opaque->scene != nullptr) {
        const int32_t layer_id = FindNodeId(opaque, node);
        const bool is_cam_prop = property_name == "visible" || property_name == "origin" ||
                                 property_name == "angles" || property_name == "zoom" ||
                                 property_name == "fov";
        if (layer_id != 0 && opaque->scene->cameraLayers.count(layer_id) != 0 && is_cam_prop) {
            // Camera layer properties must route through ApplyCameraLayerPropertyValue so
            // origin changes add the ortho/2 offset. The generic ApplyLayerPropertyValue calls
            // SetTranslate directly, which bypasses ResolveCameraLayerNodeTranslation and
            // snaps the camera node to the raw script value instead of the ortho/2 offset.
            WPSceneScriptRegistration cam_reg;
            cam_reg.object_id     = layer_id;
            cam_reg.property_name = std::string(property_name);
            cam_reg.node          = node;
            cam_reg.target_kind   = WPSceneScriptTargetKind::Camera;
            cam_reg.target_index  = 0;
            cam_reg.value_type    = hint.type;
            return JS_NewBool(context,
                              ApplyCameraLayerPropertyValue(opaque, cam_reg, runtime_value));
        }
    }

    return JS_NewBool(context, ApplyLayerPropertyValue(opaque, node, property_name, runtime_value));
}

JSValue NativeHasEffect(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 2) return JS_FALSE;

    int32_t layer_id     = 0;
    int32_t effect_index = 0;
    if (JS_ToInt32(context, &layer_id, argv[0]) != 0 ||
        JS_ToInt32(context, &effect_index, argv[1]) != 0 || effect_index < 0) {
        return JS_FALSE;
    }

    return JS_NewBool(
        context,
        opaque->scene->FindImageEffect(layer_id, static_cast<uint32_t>(effect_index)) != nullptr);
}

JSValue NativeHasEffectMember(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 3) return JS_FALSE;

    int32_t layer_id     = 0;
    int32_t effect_index = 0;
    if (JS_ToInt32(context, &layer_id, argv[0]) != 0 ||
        JS_ToInt32(context, &effect_index, argv[1]) != 0 || effect_index < 0) {
        return JS_FALSE;
    }

    std::string member_name;
    if (! ReadJSString(context, argv[2], &member_name)) return JS_FALSE;
    return JS_NewBool(
        context,
        opaque->scene->FindImageEffect(layer_id, static_cast<uint32_t>(effect_index)) != nullptr &&
            (EffectValueType(member_name).supported || member_name == "getAnimation" ||
             member_name == "getMaterial"));
}

JSValue NativeGetEffectProperty(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 3) return JS_UNDEFINED;

    int32_t layer_id     = 0;
    int32_t effect_index = 0;
    if (JS_ToInt32(context, &layer_id, argv[0]) != 0 ||
        JS_ToInt32(context, &effect_index, argv[1]) != 0 || effect_index < 0) {
        return JS_UNDEFINED;
    }

    std::string property_name;
    if (! ReadJSString(context, argv[2], &property_name)) return JS_UNDEFINED;
    const auto* effect =
        opaque->scene->FindImageEffect(layer_id, static_cast<uint32_t>(effect_index));
    if (effect == nullptr || property_name != "visible") return JS_UNDEFINED;
    return JS_NewBool(context, effect->LocalVisible());
}

JSValue NativeSetEffectProperty(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 4) return JS_FALSE;

    int32_t layer_id     = 0;
    int32_t effect_index = 0;
    if (JS_ToInt32(context, &layer_id, argv[0]) != 0 ||
        JS_ToInt32(context, &effect_index, argv[1]) != 0 || effect_index < 0) {
        return JS_FALSE;
    }

    std::string property_name;
    if (! ReadJSString(context, argv[2], &property_name)) return JS_FALSE;
    const auto hint = EffectValueType(property_name);
    if (! hint.supported) return JS_FALSE;

    const auto value = ReadDynamicValueFromJS(context, argv[3], hint.type);
    if (! value.has_value()) return JS_FALSE;

    bool visible = false;
    if (! value->tryGet(&visible)) return JS_FALSE;
    const bool applied = opaque->scene->SetEffectLocalVisibility(
        layer_id, static_cast<uint32_t>(effect_index), visible);
    if (applied) {
        LOG_INFO(
            "SceneVisibilityEffectApply: layer=%d effect-index=%d visible=%s topology-dirty=%s",
            layer_id,
            effect_index,
            visible ? "true" : "false",
            opaque->scene->renderGraphTopologyDirty ? "true" : "false");
    }
    return JS_NewBool(context, applied);
}

JSValue NativeHasEffectMaterial(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 3) return JS_FALSE;

    int32_t layer_id       = 0;
    int32_t effect_index   = 0;
    int32_t material_index = 0;
    if (JS_ToInt32(context, &layer_id, argv[0]) != 0 ||
        JS_ToInt32(context, &effect_index, argv[1]) != 0 ||
        JS_ToInt32(context, &material_index, argv[2]) != 0 || effect_index < 0 ||
        material_index < 0) {
        return JS_FALSE;
    }

    const auto target = FindEffectMaterialTarget(opaque,
                                                 layer_id,
                                                 static_cast<uint32_t>(effect_index),
                                                 static_cast<uint32_t>(material_index));
    return JS_NewBool(context, target.has_value());
}

JSValue
NativeHasEffectMaterialMember(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 4) return JS_FALSE;

    int32_t layer_id       = 0;
    int32_t effect_index   = 0;
    int32_t material_index = 0;
    if (JS_ToInt32(context, &layer_id, argv[0]) != 0 ||
        JS_ToInt32(context, &effect_index, argv[1]) != 0 ||
        JS_ToInt32(context, &material_index, argv[2]) != 0 || effect_index < 0 ||
        material_index < 0) {
        return JS_FALSE;
    }

    std::string property_name;
    if (! ReadJSString(context, argv[3], &property_name)) return JS_FALSE;

    const auto target = FindEffectMaterialTarget(opaque,
                                                 layer_id,
                                                 static_cast<uint32_t>(effect_index),
                                                 static_cast<uint32_t>(material_index));
    if (! target.has_value()) return JS_FALSE;

    // Report only properties that resolve to a real live uniform. Returning false for unknown
    // names keeps strict-mode script assignments noisy instead of silently accepting misspelled
    // material controls.
    return JS_NewBool(context,
                      FindRuntimeMaterialUniformValue(*target->material, property_name) != nullptr);
}

JSValue
NativeGetEffectMaterialProperty(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 4) return JS_UNDEFINED;

    int32_t layer_id       = 0;
    int32_t effect_index   = 0;
    int32_t material_index = 0;
    if (JS_ToInt32(context, &layer_id, argv[0]) != 0 ||
        JS_ToInt32(context, &effect_index, argv[1]) != 0 ||
        JS_ToInt32(context, &material_index, argv[2]) != 0 || effect_index < 0 ||
        material_index < 0) {
        return JS_UNDEFINED;
    }

    std::string property_name;
    if (! ReadJSString(context, argv[3], &property_name)) return JS_UNDEFINED;

    const auto target = FindEffectMaterialTarget(opaque,
                                                 layer_id,
                                                 static_cast<uint32_t>(effect_index),
                                                 static_cast<uint32_t>(material_index));
    if (! target.has_value()) return JS_UNDEFINED;

    const auto* uniform = FindRuntimeMaterialUniformValue(*target->material, property_name);
    return uniform != nullptr ? ShaderUniformValueToJS(context, *uniform) : JS_UNDEFINED;
}

JSValue
NativeSetEffectMaterialProperty(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 5) return JS_FALSE;

    int32_t layer_id       = 0;
    int32_t effect_index   = 0;
    int32_t material_index = 0;
    if (JS_ToInt32(context, &layer_id, argv[0]) != 0 ||
        JS_ToInt32(context, &effect_index, argv[1]) != 0 ||
        JS_ToInt32(context, &material_index, argv[2]) != 0 || effect_index < 0 ||
        material_index < 0) {
        return JS_FALSE;
    }

    std::string property_name;
    if (! ReadJSString(context, argv[3], &property_name)) return JS_FALSE;

    const auto target = FindEffectMaterialTarget(opaque,
                                                 layer_id,
                                                 static_cast<uint32_t>(effect_index),
                                                 static_cast<uint32_t>(material_index));
    if (! target.has_value()) return JS_FALSE;

    std::string       uniform_name;
    const auto* const current_uniform =
        FindRuntimeMaterialUniformValue(*target->material, property_name, &uniform_name);
    if (current_uniform == nullptr) {
        LOG_ERROR("SceneEffectMaterialUniformApply: layer=%d effect-index=%d material-index=%d "
                  "material='%s' property='%s' unresolved uniform='%s'",
                  layer_id,
                  effect_index,
                  material_index,
                  target->material->name.c_str(),
                  property_name.c_str(),
                  uniform_name.c_str());
        return JS_FALSE;
    }

    const auto value =
        ReadDynamicValueFromJS(context, argv[4], RuntimeDynamicTypeForShaderValue(*current_uniform));
    if (! value.has_value()) {
        LOG_ERROR("SceneEffectMaterialUniformApply: layer=%d effect-index=%d material-index=%d "
                  "material='%s' property='%s' uniform='%s' invalid JS value",
                  layer_id,
                  effect_index,
                  material_index,
                  target->material->name.c_str(),
                  property_name.c_str(),
                  uniform_name.c_str());
        return JS_FALSE;
    }

    const auto shader_value = ShaderValueFromDynamicValue(*value);
    if (! shader_value.has_value()) {
        LOG_ERROR("SceneEffectMaterialUniformApply: layer=%d effect-index=%d material-index=%d "
                  "material='%s' property='%s' uniform='%s' invalid value %s",
                  layer_id,
                  effect_index,
                  material_index,
                  target->material->name.c_str(),
                  property_name.c_str(),
                  uniform_name.c_str(),
                  value->describe().c_str());
        return JS_FALSE;
    }

    // Effect passes read SceneMaterial::customShader.constValues while drawing, so storing the
    // resolved uniform here updates the next post-process pass without rebuilding cameras or
    // changing the effect's own visibility state. This is intentionally separate from
    // setEffectProperty(), which remains limited to effect.visible.
    target->material->customShader.constValues[uniform_name] =
        ShapeScalarShaderUniformValue(*target->material, uniform_name, *shader_value);
    LOG_INFO("SceneEffectMaterialUniformApply: layer=%d effect-index=%d effect-id=%d effect='%s' "
             "material-index=%d material='%s' property='%s' uniform='%s' value=%s",
             layer_id,
             effect_index,
             target->effect != nullptr ? target->effect->EffectId() : 0,
             target->effect != nullptr ? target->effect->EffectName().c_str() : "",
             material_index,
             target->material->name.c_str(),
             property_name.c_str(),
             uniform_name.c_str(),
             value->describe().c_str());
    return JS_TRUE;
}

JSValue NativeHasLayerMember(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (argc < 2) return JS_FALSE;

    int32_t node_id = 0;
    if (opaque != nullptr && JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_FALSE;

    std::string member_name;
    if (! ReadJSString(context, argv[1], &member_name)) return JS_FALSE;
    if (member_name == "originalOrigin") {
        // Report originalOrigin as a real layer API only when the layer still has an initial
        // configuration record. That keeps deleted/dynamic lookup failures observable while
        // allowing reset-position scripts to test for originalOrigin just like in Wallpaper Engine.
        return JS_NewBool(context,
                          opaque != nullptr && opaque->scene != nullptr &&
                              opaque->scene->initialLayerConfigJson.contains(node_id));
    }
    if (member_name == "size") {
        return JS_NewBool(context,
                          opaque != nullptr && (FindImageLayerById(opaque, node_id) != nullptr ||
                                                FindTextLayerById(opaque, node_id) != nullptr));
    }
    if (member_name == "getAttachmentIndex" || member_name == "getAttachmentMatrix" ||
        member_name == "getAttachmentOrigin" || member_name == "getAttachmentAngles" ||
        member_name == "getBoneCount" || member_name == "getBoneIndex" ||
        member_name == "getBoneParentIndex" || member_name == "getBoneTransform" ||
        member_name == "getLocalBoneTransform" || member_name == "getLocalBoneAngles" ||
        member_name == "getLocalBoneOrigin" || member_name == "setBoneTransform" ||
        member_name == "setLocalBoneTransform" || member_name == "setLocalBoneAngles" ||
        member_name == "setLocalBoneOrigin" || member_name == "applyBonePhysicsImpulse") {
        auto* node = opaque != nullptr ? FindNodeById(opaque, node_id) : nullptr;
        return JS_NewBool(context, AdvanceNodePuppetForScriptQuery(opaque, node) != nullptr);
    }
    if (member_name == "getTextureAnimation" || member_name == "getVideoTexture" ||
        member_name == "getAnimation" || member_name == "getAnimationLayerCount" ||
        member_name == "getAnimationLayer" || member_name == "getParent" ||
        member_name == "getChildren" || member_name == "rotateObjectSpace") {
        return JS_TRUE;
    }
    if (member_name == "setParent") {
        return JS_NewBool(context, opaque != nullptr && FindNodeById(opaque, node_id) != nullptr);
    }

    if (opaque != nullptr) {
        if (const auto sound_handle = FindSoundHandleByLayerId(opaque, node_id);
            sound_handle.has_value()) {
            return JS_NewBool(context,
                              member_name == "name" || member_name == "volume" ||
                                  member_name == "play" || member_name == "stop" ||
                                  member_name == "pause" || member_name == "isPlaying");
        }
        if (FindTextLayerById(opaque, node_id) != nullptr) {
            return JS_NewBool(context,
                              HasTextLayerProperty(member_name) ||
                                  LayerValueType(member_name).supported || member_name == "name");
        }
    }

    if (member_name == "getTransformMatrix") return JS_TRUE;
    return JS_NewBool(context, LayerValueType(member_name).supported || member_name == "name");
}

JSValue NativeGetLayerRelation(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_NewInt32(context, 0);

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_NewInt32(context, 0);

    std::string relation_name;
    if (! ReadJSString(context, argv[1], &relation_name)) return JS_NewInt32(context, 0);

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr || relation_name != "parent") return JS_NewInt32(context, 0);
    if (opaque->scene == nullptr) return JS_NewInt32(context, 0);
    const auto binding = opaque->scene->GetLayerParentBinding(node_id);
    if (binding.parent_id != 0) return JS_NewInt32(context, binding.parent_id);

    // Some imported scenes preserve hierarchy in the runtime node tree without a script parent
    // binding. Fall back to that graph so getParent() can still provide a useful transform.
    if (auto* parent_node = node->Parent()) {
        const auto owner_id = FindOwningLayerId(opaque, parent_node);
        if (owner_id != 0 && owner_id != node_id) return JS_NewInt32(context, owner_id);
        const auto parent_id = FindNodeId(opaque, parent_node);
        if (parent_id != 0 && parent_id != node_id) return JS_NewInt32(context, parent_id);
    }

    return JS_NewInt32(context, 0);
}

JSValue NativeGetLayerChildren(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_NewArray(context);

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_NewArray(context);

    auto*   node  = FindNodeById(opaque, node_id);
    JSValue array = JS_NewArray(context);
    if (node == nullptr || opaque->scene == nullptr) return array;

    uint32_t index = 0;
    for (const auto layer_id : opaque->scene->GetLayerChildren(node_id)) {
        JS_SetPropertyUint32(context, array, index++, JS_NewInt32(context, layer_id));
    }
    return array;
}

JSValue NativeGetSceneLayerCount(JSContext* context, JSValueConst, int, JSValueConst*) {
    auto* opaque = GetOpaque(context);
    return JS_NewInt32(context,
                       opaque != nullptr && opaque->scene != nullptr
                           ? static_cast<int32_t>(opaque->scene->layerOrder.size())
                           : 0);
}

JSValue NativeEnumerateSceneLayers(JSContext* context, JSValueConst, int, JSValueConst*) {
    auto*   opaque = GetOpaque(context);
    JSValue array  = JS_NewArray(context);
    if (opaque == nullptr || opaque->scene == nullptr) return array;

    uint32_t index = 0;
    for (const auto layer_id : opaque->scene->layerOrder) {
        JS_SetPropertyUint32(context, array, index++, JS_NewInt32(context, layer_id));
    }
    return array;
}

JSValue NativeGetSceneProperty(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_UNDEFINED;

    std::string property_name;
    if (! ReadJSString(context, argv[0], &property_name)) return JS_UNDEFINED;

    const auto value = ReadScenePropertyValue(opaque, property_name);
    if (! value.has_value()) return JS_UNDEFINED;
    const auto script_value = value->toScriptValue();
    return script_value.has_value() ? ScriptValueToJS(context, *script_value) : JS_UNDEFINED;
}

JSValue NativeSetSceneProperty(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_FALSE;

    std::string property_name;
    if (! ReadJSString(context, argv[0], &property_name)) return JS_FALSE;

    const auto hint = SceneValueType(property_name);
    if (! hint.supported) return JS_FALSE;

    const auto value = ReadDynamicValueFromJS(context, argv[1], hint.type);
    if (! value.has_value()) return JS_FALSE;

    return JS_NewBool(context, ApplyScenePropertyValue(opaque, property_name, *value));
}

JSValue NativeHasSceneMember(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_FALSE;

    std::string property_name;
    if (! ReadJSString(context, argv[0], &property_name)) return JS_FALSE;
    return JS_NewBool(context, SceneValueType(property_name).supported);
}

JSValue NativeGetSceneLayer(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 1) return JS_NewInt32(context, 0);

    int32_t index = 0;
    if (JS_IsNumber(argv[0]) && JS_ToInt32(context, &index, argv[0]) == 0) {
        if (index >= 0 && static_cast<size_t>(index) < opaque->scene->layerOrder.size()) {
            return JS_NewInt32(context, opaque->scene->layerOrder[static_cast<size_t>(index)]);
        }
    }

    std::string layer_name;
    if (! ReadJSString(context, argv[0], &layer_name)) return JS_NewInt32(context, 0);

    auto it = opaque->scene->layerNameToId.find(layer_name);
    return JS_NewInt32(context, it == opaque->scene->layerNameToId.end() ? 0 : it->second);
}

JSValue NativeGetSceneLayerIndex(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 1) return JS_NewInt32(context, -1);

    const auto resolved_layer = ResolveLayerReference(context, opaque, argv[0]);
    if (! resolved_layer.has_value()) return JS_NewInt32(context, -1);
    const int32_t layer_id = *resolved_layer;

    const auto it =
        std::find(opaque->scene->layerOrder.begin(), opaque->scene->layerOrder.end(), layer_id);
    if (it == opaque->scene->layerOrder.end()) return JS_NewInt32(context, -1);
    return JS_NewInt32(context,
                       static_cast<int32_t>(std::distance(opaque->scene->layerOrder.begin(), it)));
}

JSValue NativeGetInitialSceneLayerConfig(JSContext* context, JSValueConst, int argc,
                                         JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 1) return JS_UNDEFINED;

    const auto resolved_layer = ResolveLayerReference(context, opaque, argv[0]);
    if (! resolved_layer.has_value()) return JS_UNDEFINED;
    const int32_t layer_id = *resolved_layer;

    const auto it = opaque->scene->initialLayerConfigJson.find(layer_id);
    if (it == opaque->scene->initialLayerConfigJson.end()) return JS_UNDEFINED;
    return ParseJsonToJS(context, it->second);
}

JSValue NativeDestroySceneLayer(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_FALSE;

    const auto resolved_layer = ResolveLayerReference(context, opaque, argv[0]);
    if (! resolved_layer.has_value()) return JS_FALSE;

    const int32_t layer_id = *resolved_layer;
    if (std::find(opaque->pending_destroy_layer_ids.begin(),
                  opaque->pending_destroy_layer_ids.end(),
                  layer_id) == opaque->pending_destroy_layer_ids.end()) {
        opaque->pending_destroy_layer_ids.push_back(layer_id);
    }
    return JS_TRUE;
}

JSValue NativeCreateSceneLayer(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 1) return JS_NewInt32(context, 0);

    auto create_layer_json = JsonFromJS(context, argv[0]);
    if (! create_layer_json.has_value()) return JS_NewInt32(context, 0);

    nlohmann::json normalized_json;
    if (create_layer_json->is_string() || IsAssetHandleJson(*create_layer_json)) {
        auto config = MaterializeAssetHandleConfig(opaque->scene, *create_layer_json);
        if (! config.has_value()) return JS_NewInt32(context, 0);
        normalized_json = *config;
    } else if (create_layer_json->is_object()) {
        normalized_json = NormalizeCreateLayerJson(*create_layer_json);
    } else {
        return JS_NewInt32(context, 0);
    }

    if (normalized_json.contains("parent")) {
        if (normalized_json.at("parent").is_string()) {
            const auto layer_name = normalized_json.at("parent").get<std::string>();
            auto       layer_it   = opaque->scene->layerNameToId.find(layer_name);
            if (layer_it != opaque->scene->layerNameToId.end()) {
                normalized_json["parent"] = layer_it->second;
            }
        } else if (normalized_json.at("parent").is_number_integer()) {
            const int32_t parent_id = normalized_json.at("parent").get<int32_t>();
            if (opaque->scene->layerNodes.count(parent_id) == 0) {
                normalized_json["parent"] = 0;
            }
        }
    }

    std::vector<WPSceneScriptRegistration> binding_registrations;
    std::vector<WPSceneScriptRegistration> property_animation_registrations;
    std::vector<WPSceneScriptRegistration> script_registrations;
    int32_t                                layer_id { 0 };
    if (! CreateDynamicSceneLayer(*opaque->scene,
                                  normalized_json,
                                  &opaque->user_properties,
                                  &binding_registrations,
                                  &script_registrations,
                                  &property_animation_registrations,
                                  nullptr,
                                  &layer_id)) {
        return JS_NewInt32(context, 0);
    }

    if (opaque->scene->scriptHost) {
        for (const auto& registration : binding_registrations) {
            opaque->scene->scriptHost->RegisterPropertyBinding(registration);
        }
        for (const auto& registration : property_animation_registrations) {
            opaque->scene->scriptHost->RegisterPropertyAnimation(registration);
        }
        for (const auto& registration : script_registrations) {
            opaque->scene->scriptHost->RegisterPropertyScript(registration);
        }
        opaque->scene->scriptHost->ApplyUserProperties(opaque->user_properties, false);
    }
    opaque->scene->MarkRenderGraphTopologyDirty();
    ResortLayerTree(opaque->scene->sceneGraph.get(), opaque);

    return JS_NewInt32(context, layer_id);
}

JSValue NativeSortSceneLayer(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 2) return JS_FALSE;

    const auto resolved_layer = ResolveLayerReference(context, opaque, argv[0]);
    if (! resolved_layer.has_value()) return JS_FALSE;

    int32_t target_index = 0;
    if (JS_ToInt32(context, &target_index, argv[1]) != 0) return JS_FALSE;

    auto& layer_order = opaque->scene->layerOrder;
    auto  current_it  = std::find(layer_order.begin(), layer_order.end(), *resolved_layer);
    if (current_it == layer_order.end()) return JS_FALSE;

    const int32_t layer_id = *resolved_layer;
    layer_order.erase(current_it);
    target_index = std::clamp(target_index, 0, static_cast<int32_t>(layer_order.size()));
    layer_order.insert(layer_order.begin() + target_index, layer_id);
    opaque->scene->MarkRenderGraphTopologyDirty();
    ResortLayerTree(opaque->scene->sceneGraph.get(), opaque);
    return JS_TRUE;
}

JSValue NativeResolveLayerAnimation(JSContext* context, JSValueConst, int argc,
                                    JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_NewInt32(context, 0);

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_NewInt32(context, 0);

    std::string property_name;
    if (! ReadJSString(context, argv[1], &property_name)) return JS_NewInt32(context, 0);

    auto* animation = FindPropertyAnimation(opaque, FindNodeById(opaque, node_id), property_name);
    return JS_NewInt32(context,
                       animation != nullptr ? static_cast<int32_t>(animation->animation_id) : 0);
}

JSValue NativeResolvePropertyAnimation(JSContext* context, JSValueConst, int argc,
                                       JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_NewInt32(context, 0);

    int32_t instance_id = 0;
    if (JS_ToInt32(context, &instance_id, argv[0]) != 0) return JS_NewInt32(context, 0);

    std::optional<std::string> property_name;
    if (argc >= 2 && ! JS_IsUndefined(argv[1]) && ! JS_IsNull(argv[1])) {
        std::string name;
        if (! ReadJSString(context, argv[1], &name)) return JS_NewInt32(context, 0);
        property_name = std::move(name);
    }

    auto* animation =
        FindPropertyAnimation(opaque, static_cast<uint32_t>(instance_id), property_name);
    return JS_NewInt32(context,
                       animation != nullptr ? static_cast<int32_t>(animation->animation_id) : 0);
}

JSValue NativeGetPropertyAnimationProperty(JSContext* context, JSValueConst, int argc,
                                           JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_UNDEFINED;

    int32_t animation_id = 0;
    if (JS_ToInt32(context, &animation_id, argv[0]) != 0) return JS_UNDEFINED;

    std::string property_name;
    if (! ReadJSString(context, argv[1], &property_name)) return JS_UNDEFINED;

    const auto* animation = FindPropertyAnimation(opaque, static_cast<uint32_t>(animation_id));
    if (animation == nullptr || animation->registration.animation == nullptr) return JS_UNDEFINED;

    const auto& definition = *animation->registration.animation;
    if (property_name == "fps") return JS_NewFloat64(context, definition.fps);
    if (property_name == "frameCount") {
        return JS_NewInt32(context, static_cast<int32_t>(std::lround(definition.frame_count)));
    }
    if (property_name == "duration") {
        const double duration =
            definition.fps > 0.0 ? definition.frame_count / definition.fps : 0.0;
        return JS_NewFloat64(context, duration);
    }
    if (property_name == "name") {
        // Expose the Wallpaper Engine timeline label when it exists; falling back to the property
        // name keeps older script probes useful for animations authored before name parsing was
        // implemented.
        const std::string& exposed_name =
            !definition.name.empty() ? definition.name : animation->registration.property_name;
        return JS_NewStringLen(context,
                               exposed_name.c_str(),
                               exposed_name.size());
    }
    if (property_name == "rate") return JS_NewFloat64(context, animation->state.rate);
    return JS_UNDEFINED;
}

JSValue NativeSetPropertyAnimationProperty(JSContext* context, JSValueConst, int argc,
                                           JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 3) return JS_FALSE;

    int32_t animation_id = 0;
    if (JS_ToInt32(context, &animation_id, argv[0]) != 0) return JS_FALSE;

    std::string property_name;
    if (! ReadJSString(context, argv[1], &property_name)) return JS_FALSE;

    auto* animation = FindPropertyAnimation(opaque, static_cast<uint32_t>(animation_id));
    if (animation == nullptr) return JS_FALSE;

    if (property_name == "rate") {
        double rate = 1.0;
        if (JS_ToFloat64(context, &rate, argv[2]) != 0) return JS_FALSE;
        animation->state.rate = rate;
        return JS_TRUE;
    }

    return JS_FALSE;
}

JSValue NativePropertyAnimationCall(JSContext* context, JSValueConst, int argc,
                                    JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_UNDEFINED;

    int32_t animation_id = 0;
    if (JS_ToInt32(context, &animation_id, argv[0]) != 0) return JS_UNDEFINED;

    std::string command;
    if (! ReadJSString(context, argv[1], &command)) return JS_UNDEFINED;

    auto* animation = FindPropertyAnimation(opaque, static_cast<uint32_t>(animation_id));
    if (animation == nullptr || animation->registration.animation == nullptr) return JS_UNDEFINED;

    const auto frame_count = std::max(animation->registration.animation->frame_count, 0.0);
    if (command == "play") {
        // Single-shot WE timelines are commonly triggered from cursorClick handlers. Once such an
        // animation has reached its terminal frame, a later play() call should start a new pulse
        // instead of resuming from the already-finished boundary and stopping again immediately.
        if (animation->registration.animation->mode == WPPropertyAnimationMode::Single &&
            animation->state.frame >= frame_count) {
            animation->state.frame = 0.0;
            ApplyPropertyAnimationInstance(opaque, *animation);
        }
        animation->state.playing = true;
        return JS_UNDEFINED;
    }
    if (command == "stop") {
        animation->state.playing = false;
        animation->state.frame   = 0.0;
        ApplyPropertyAnimationInstance(opaque, *animation);
        return JS_UNDEFINED;
    }
    if (command == "pause") {
        animation->state.playing = false;
        return JS_UNDEFINED;
    }
    if (command == "isPlaying") {
        return JS_NewBool(context, animation->state.playing);
    }
    if (command == "getFrame") {
        return JS_NewFloat64(context, animation->state.frame);
    }
    if (command == "setFrame") {
        if (argc < 3) return JS_UNDEFINED;
        double frame = 0.0;
        if (JS_ToFloat64(context, &frame, argv[2]) == 0) {
            animation->state.frame = std::clamp(frame, 0.0, frame_count);
            ApplyPropertyAnimationInstance(opaque, *animation);
        }
        return JS_UNDEFINED;
    }

    return JS_UNDEFINED;
}

JSValue NativeLayerCall(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_UNDEFINED;

    int32_t layer_id = 0;
    if (JS_ToInt32(context, &layer_id, argv[0]) != 0) return JS_UNDEFINED;

    std::string command;
    if (! ReadJSString(context, argv[1], &command)) return JS_UNDEFINED;

    if (command == "setParent") {
        auto* node = FindNodeById(opaque, layer_id);
        if (node == nullptr) return JS_FALSE;

        SceneNode* new_parent = nullptr;
        if (argc >= 3 && ! JS_IsUndefined(argv[2]) && ! JS_IsNull(argv[2])) {
            const auto resolved_parent = ResolveLayerReference(context, opaque, argv[2]);
            if (! resolved_parent.has_value()) return JS_FALSE;
            new_parent = FindNodeById(opaque, *resolved_parent);
            if (new_parent == nullptr || new_parent == node) return JS_FALSE;
            if (IsLogicalAncestor(opaque, new_parent, node)) return JS_FALSE;
        }

        std::optional<ScriptAttachmentBinding> attachment_binding;
        bool                                   adjust_transforms = false;
        if (argc >= 4 && ! JS_IsUndefined(argv[3]) && ! JS_IsNull(argv[3])) {
            if (JS_IsBool(argv[3])) {
                const int adjust = JS_ToBool(context, argv[3]);
                if (adjust < 0) return JS_FALSE;
                adjust_transforms = adjust != 0;
            } else {
                if (new_parent == nullptr) return JS_FALSE;
                const auto* parent_puppet = AdvanceNodePuppetForScriptQuery(opaque, new_parent);
                if (parent_puppet == nullptr) return JS_FALSE;
                attachment_binding =
                    ResolveAttachmentBindingForScript(context, argv[3], *parent_puppet);
                if (! attachment_binding.has_value()) return JS_FALSE;
                if (argc >= 5 && ! JS_IsUndefined(argv[4]) && ! JS_IsNull(argv[4])) {
                    const int adjust = JS_ToBool(context, argv[4]);
                    if (adjust < 0) return JS_FALSE;
                    adjust_transforms = adjust != 0;
                }
            }
        }

        return JS_NewBool(
            context,
            RebindLayerParent(
                opaque, node, new_parent, std::move(attachment_binding), adjust_transforms));
    }

    if (opaque->scene != nullptr && opaque->scene->soundManager != nullptr) {
        const auto sound_handle = FindSoundHandleByLayerId(opaque, layer_id);
        if (sound_handle.has_value()) {
            if (command == "play") {
                // Sound play/stop/pause is script-driven for music selection layers.  Logging each
                // command with the resolved handle makes run.log show whether silence comes from the
                // script bridge, SoundManager state, or a later decoder/stream failure.
                const bool applied = opaque->scene->soundManager->Play(*sound_handle);
                LOG_INFO("SceneSoundCall: layer=%d command='play' handle=%u applied=%s playing=%s",
                         layer_id,
                         *sound_handle,
                         applied ? "true" : "false",
                         opaque->scene->soundManager->IsPlaying(*sound_handle) ? "true" : "false");
                return JS_UNDEFINED;
            }
            if (command == "stop") {
                const bool applied = opaque->scene->soundManager->Stop(*sound_handle);
                LOG_INFO("SceneSoundCall: layer=%d command='stop' handle=%u applied=%s playing=%s",
                         layer_id,
                         *sound_handle,
                         applied ? "true" : "false",
                         opaque->scene->soundManager->IsPlaying(*sound_handle) ? "true" : "false");
                return JS_UNDEFINED;
            }
            if (command == "pause") {
                const bool applied = opaque->scene->soundManager->Pause(*sound_handle);
                LOG_INFO("SceneSoundCall: layer=%d command='pause' handle=%u applied=%s playing=%s",
                         layer_id,
                         *sound_handle,
                         applied ? "true" : "false",
                         opaque->scene->soundManager->IsPlaying(*sound_handle) ? "true" : "false");
                return JS_UNDEFINED;
            }
            if (command == "isPlaying") {
                return JS_NewBool(context, opaque->scene->soundManager->IsPlaying(*sound_handle));
            }
        }
    }

    auto* node = FindNodeById(opaque, layer_id);
    if (node == nullptr) return JS_UNDEFINED;

    if (command == "getTransformMatrix") {
        // Layer transform queries are supported for every scene node, not only puppet-backed
        // model layers. Update the node transform and return WE's column-major .m layout so
        // scripts that compare parent.m[13] against child.m[13] see the expected coordinates.
        node->UpdateTrans();
        return TransformMatrixToJS(context, node->ModelTrans());
    }

    const auto* puppet = AdvanceNodePuppetForScriptQuery(opaque, node);
    if (puppet == nullptr) return JS_UNDEFINED;

    if (command == "getAttachmentIndex") {
        if (argc < 3) return JS_NewInt32(context, -1);
        const auto attachment = ResolveAttachmentReference(context, argv[2], *puppet);
        return JS_NewInt32(context,
                           attachment.has_value() ? static_cast<int32_t>(attachment->index) : -1);
    }
    if (command == "getAttachmentMatrix") {
        if (argc < 3) return JS_UNDEFINED;
        const auto attachment = ResolveAttachmentReference(context, argv[2], *puppet);
        if (! attachment.has_value() || attachment->attachment == nullptr) return JS_UNDEFINED;
        const auto transform = GetAttachmentWorldTransform(opaque, node, *attachment->attachment);
        return transform.has_value() ? Matrix4ToJS(context, *transform) : JS_UNDEFINED;
    }
    if (command == "getAttachmentOrigin") {
        if (argc < 3) return JS_UNDEFINED;
        const auto attachment = ResolveAttachmentReference(context, argv[2], *puppet);
        if (! attachment.has_value() || attachment->attachment == nullptr) return JS_UNDEFINED;
        const auto transform = GetAttachmentWorldTransform(opaque, node, *attachment->attachment);
        if (! transform.has_value()) return JS_UNDEFINED;
        const auto translation =
            std::array<double, 3> { (*transform)(0, 3), (*transform)(1, 3), (*transform)(2, 3) };
        return Vec3ToJS(context, translation);
    }
    if (command == "getAttachmentAngles") {
        if (argc < 3) return JS_UNDEFINED;
        const auto attachment = ResolveAttachmentReference(context, argv[2], *puppet);
        if (! attachment.has_value() || attachment->attachment == nullptr) return JS_UNDEFINED;
        const auto transform = GetAttachmentWorldTransform(opaque, node, *attachment->attachment);
        if (! transform.has_value()) return JS_UNDEFINED;
        // Puppet angle helpers are part of the SceneScript surface, so they expose degrees even
        // though the underlying affine decomposition is stored and composed in radians.
        return Vec3ToJS(
            context,
            ConvertEulerArrayScale(AffineEulerAngles(Eigen::Affine3f(transform->cast<float>())),
                                   kSceneScriptRadiansToDegrees));
    }
    if (command == "getBoneCount") {
        return JS_NewInt32(context, static_cast<int32_t>(puppet->bones.size()));
    }
    if (command == "getBoneIndex") {
        if (argc < 3) return JS_NewInt32(context, -1);
        std::string bone_name;
        if (! ReadJSString(context, argv[2], &bone_name)) return JS_NewInt32(context, -1);
        const auto bone_index = puppet->FindBoneIndex(bone_name);
        return JS_NewInt32(context,
                           bone_index == 0xFFFFFFFFu ? -1 : static_cast<int32_t>(bone_index));
    }
    if (command == "getBoneParentIndex") {
        if (argc < 3) return JS_NewInt32(context, -1);
        const auto bone_index = ResolveBoneReference(context, argv[2], *puppet);
        if (! bone_index.has_value()) return JS_NewInt32(context, -1);
        const auto parent_index = puppet->bones[*bone_index].file_parent;
        return JS_NewInt32(context,
                           parent_index == 0xFFFFFFFFu ? -1 : static_cast<int32_t>(parent_index));
    }
    if (command == "getBoneTransform") {
        if (argc < 3) return JS_UNDEFINED;
        const auto bone_index = ResolveBoneReference(context, argv[2], *puppet);
        if (! bone_index.has_value()) return JS_UNDEFINED;
        const auto transform = GetBoneModelTransform(opaque, node, *bone_index);
        if (! transform.has_value()) return JS_UNDEFINED;
        node->UpdateTrans();
        return Matrix4ToJS(context, node->ModelTrans() * transform->matrix().cast<double>());
    }
    if (command == "getLocalBoneTransform") {
        if (argc < 3) return JS_UNDEFINED;
        const auto bone_index = ResolveBoneReference(context, argv[2], *puppet);
        if (! bone_index.has_value()) return JS_UNDEFINED;
        const auto transform = GetBoneLocalTransform(opaque, node, *bone_index);
        return transform.has_value() ? Matrix4ToJS(context, transform->matrix().cast<double>())
                                     : JS_UNDEFINED;
    }
    if (command == "getLocalBoneAngles") {
        if (argc < 3) return JS_UNDEFINED;
        const auto bone_index = ResolveBoneReference(context, argv[2], *puppet);
        if (! bone_index.has_value()) return JS_UNDEFINED;
        const auto transform = GetBoneLocalTransform(opaque, node, *bone_index);
        return transform.has_value()
                   ? Vec3ToJS(context,
                              ConvertEulerArrayScale(AffineEulerAngles(*transform),
                                                     kSceneScriptRadiansToDegrees))
                   : JS_UNDEFINED;
    }
    if (command == "getLocalBoneOrigin") {
        if (argc < 3) return JS_UNDEFINED;
        const auto bone_index = ResolveBoneReference(context, argv[2], *puppet);
        if (! bone_index.has_value()) return JS_UNDEFINED;
        const auto transform = GetBoneLocalTransform(opaque, node, *bone_index);
        return transform.has_value() ? Vec3ToJS(context, AffineTranslation(*transform))
                                     : JS_UNDEFINED;
    }
    if (command == "setLocalBoneTransform") {
        if (argc < 4) return JS_FALSE;
        const auto bone_index = ResolveBoneReference(context, argv[2], *puppet);
        const auto transform  = ReadMatrix4FromJS(context, argv[3]);
        if (! bone_index.has_value() || ! transform.has_value()) return JS_FALSE;
        return JS_NewBool(
            context,
            SetBoneLocalTransform(
                opaque, node, *bone_index, Eigen::Affine3f(transform->cast<float>())));
    }
    if (command == "setBoneTransform") {
        if (argc < 4) return JS_FALSE;
        const auto bone_index   = ResolveBoneReference(context, argv[2], *puppet);
        const auto world_matrix = ReadMatrix4FromJS(context, argv[3]);
        if (! bone_index.has_value() || ! world_matrix.has_value()) return JS_FALSE;

        node->UpdateTrans();
        const Eigen::Matrix4d layer_inverse = node->ModelTrans().inverse();
        const Eigen::Affine3f desired_model((layer_inverse * *world_matrix).cast<float>());

        Eigen::Affine3f local_transform = desired_model;
        const auto      parent_index    = puppet->bones[*bone_index].anim_parent;
        if (parent_index != WPPuppet::NO_PARENT && parent_index < puppet->bones.size()) {
            auto parent_transform = GetBoneModelTransform(opaque, node, parent_index);
            if (! parent_transform.has_value()) return JS_FALSE;
            if (puppet->world_anchored_bones) {
                *parent_transform = *parent_transform * puppet->bones[parent_index].inv_bind.matrix();
            }
            local_transform = parent_transform->inverse() * desired_model;
        }

        return JS_NewBool(context,
                          SetBoneLocalTransform(opaque, node, *bone_index, local_transform));
    }
    if (command == "setLocalBoneAngles") {
        if (argc < 4) return JS_FALSE;
        const auto bone_index = ResolveBoneReference(context, argv[2], *puppet);
        if (! bone_index.has_value()) return JS_FALSE;

        const auto angles = ReadDynamicValueFromJS(context, argv[3], WPDynamicValue::Type::Float3);
        const auto current_transform = GetBoneLocalTransform(opaque, node, *bone_index);
        if (! angles.has_value() || ! current_transform.has_value()) return JS_FALSE;

        const auto runtime_angles = ConvertFloat3Scale(*angles, kSceneScriptDegreesToRadians);
        std::array<float, 3> angle_values {};
        if (! runtime_angles.tryGet(&angle_values)) return JS_FALSE;

        Eigen::Vector3f translation {};
        Eigen::Vector3f rotation {};
        Eigen::Vector3f scale {};
        DecomposeAffine(*current_transform, translation, rotation, scale);
        rotation = Eigen::Vector3f(angle_values[0], angle_values[1], angle_values[2]);

        return JS_NewBool(
            context,
            SetBoneLocalTransform(
                opaque, node, *bone_index, ComposeAffine(translation, rotation, scale)));
    }
    if (command == "setLocalBoneOrigin") {
        if (argc < 4) return JS_FALSE;
        const auto bone_index = ResolveBoneReference(context, argv[2], *puppet);
        if (! bone_index.has_value()) return JS_FALSE;

        const auto origin = ReadDynamicValueFromJS(context, argv[3], WPDynamicValue::Type::Float3);
        const auto current_transform = GetBoneLocalTransform(opaque, node, *bone_index);
        if (! origin.has_value() || ! current_transform.has_value()) return JS_FALSE;

        std::array<float, 3> origin_values {};
        if (! origin->tryGet(&origin_values)) return JS_FALSE;

        Eigen::Vector3f translation {};
        Eigen::Vector3f rotation {};
        Eigen::Vector3f scale {};
        DecomposeAffine(*current_transform, translation, rotation, scale);
        translation = Eigen::Vector3f(origin_values[0], origin_values[1], origin_values[2]);

        return JS_NewBool(
            context,
            SetBoneLocalTransform(
                opaque, node, *bone_index, ComposeAffine(translation, rotation, scale)));
    }
    if (command == "applyBonePhysicsImpulse") {
        if (argc < 4) return JS_FALSE;
        const auto bone_index = ResolveBoneReference(context, argv[2], *puppet);
        const auto impulse = ReadDynamicValueFromJS(context, argv[3], WPDynamicValue::Type::Float3);
        const auto current_transform = bone_index.has_value()
                                           ? GetBoneLocalTransform(opaque, node, *bone_index)
                                           : std::nullopt;
        if (! bone_index.has_value() || ! impulse.has_value() || ! current_transform.has_value())
            return JS_FALSE;

        std::array<float, 3> impulse_values {};
        if (! impulse->tryGet(&impulse_values)) return JS_FALSE;

        Eigen::Vector3f translation {};
        Eigen::Vector3f rotation {};
        Eigen::Vector3f scale {};
        DecomposeAffine(*current_transform, translation, rotation, scale);
        translation += Eigen::Vector3f(impulse_values[0], impulse_values[1], impulse_values[2]);

        return JS_NewBool(
            context,
            SetBoneLocalTransform(
                opaque, node, *bone_index, ComposeAffine(translation, rotation, scale)));
    }

    return JS_UNDEFINED;
}

JSValue NativeSetTimer(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 4) return JS_NewInt32(context, 0);

    int32_t instance_id = 0;
    double  delay_ms    = 0.0;
    int     repeat      = 0;
    if (JS_ToInt32(context, &instance_id, argv[0]) != 0 || ! JS_IsFunction(context, argv[1]) ||
        JS_ToFloat64(context, &delay_ms, argv[2]) != 0 || JS_ToBool(context, argv[3]) < 0) {
        return JS_NewInt32(context, 0);
    }
    repeat = JS_ToBool(context, argv[3]);

    ScriptTimer timer;
    timer.id                = opaque->next_timer_id++;
    timer.owner_instance_id = static_cast<uint32_t>(instance_id);
    timer.remaining_ms      = std::max(0.0, delay_ms);
    timer.interval_ms       = std::max(0.0, delay_ms);
    timer.repeat            = repeat != 0;
    timer.callback          = JS_DupValue(context, argv[1]);
    opaque->timers.push_back(timer);

    return JS_NewInt64(context, static_cast<int64_t>(timer.id));
}

JSValue NativeClearTimer(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_FALSE;

    int64_t timer_id = 0;
    if (JS_ToInt64(context, &timer_id, argv[0]) != 0) return JS_FALSE;

    auto& timers = opaque->timers;
    timers.erase(std::remove_if(timers.begin(),
                                timers.end(),
                                [context, timer_id](ScriptTimer& timer) {
                                    if (timer.id != static_cast<uint64_t>(timer_id)) return false;
                                    FreeJSValue(context, timer.callback);
                                    return true;
                                }),
                 timers.end());
    return JS_TRUE;
}

JSValue NativeLocalStorageGet(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_UNDEFINED;

    std::string key;
    if (! ReadJSString(context, argv[0], &key)) return JS_UNDEFINED;

    std::string location_name { "screen" };
    if (argc >= 2 && ! JS_IsUndefined(argv[1]) && ! JS_IsNull(argv[1])) {
        if (! ReadJSString(context, argv[1], &location_name)) return JS_UNDEFINED;
    }

    auto* bucket = ResolveLocalStorageBucket(opaque, ResolveLocalStorageLocation(location_name));
    if (! EnsureLocalStorageBucketObject(bucket, location_name)) return JS_UNDEFINED;

    const auto it = bucket->find(key);
    if (it == bucket->end()) return JS_UNDEFINED;
    return ParseJsonToJS(context, it->dump());
}

JSValue NativeLocalStorageSet(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_FALSE;

    std::string key;
    if (! ReadJSString(context, argv[0], &key)) return JS_FALSE;

    std::string location_name { "screen" };
    if (argc >= 3 && ! JS_IsUndefined(argv[2]) && ! JS_IsNull(argv[2])) {
        if (! ReadJSString(context, argv[2], &location_name)) return JS_FALSE;
    }

    const auto value = JsonFromJS(context, argv[1]);
    auto* bucket = ResolveLocalStorageBucket(opaque, ResolveLocalStorageLocation(location_name));
    if (! value.has_value() || ! EnsureLocalStorageBucketObject(bucket, location_name))
        return JS_FALSE;

    (*bucket)[key] = *value;
    SaveLocalStorage(opaque);
    return JS_TRUE;
}

JSValue NativeLocalStorageClear(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr) return JS_FALSE;

    std::string location_name { "screen" };
    if (argc >= 1 && ! JS_IsUndefined(argv[0]) && ! JS_IsNull(argv[0])) {
        if (! ReadJSString(context, argv[0], &location_name)) return JS_FALSE;
    }

    auto* bucket = ResolveLocalStorageBucket(opaque, ResolveLocalStorageLocation(location_name));
    if (! EnsureLocalStorageBucketObject(bucket, location_name)) return JS_FALSE;

    *bucket = nlohmann::json::object();
    SaveLocalStorage(opaque);
    return JS_TRUE;
}

JSValue NativeLocalStorageDelete(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_FALSE;

    std::string key;
    if (! ReadJSString(context, argv[0], &key)) return JS_FALSE;

    std::string location_name { "screen" };
    if (argc >= 2 && ! JS_IsUndefined(argv[1]) && ! JS_IsNull(argv[1])) {
        if (! ReadJSString(context, argv[1], &location_name)) return JS_FALSE;
    }

    auto* bucket = ResolveLocalStorageBucket(opaque, ResolveLocalStorageLocation(location_name));
    if (! EnsureLocalStorageBucketObject(bucket, location_name)) return JS_FALSE;

    const bool erased = bucket->erase(key) > 0;
    if (erased) SaveLocalStorage(opaque);
    return JS_NewBool(context, erased);
}

JSValue NativeRegisterAudioBuffers(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_UNDEFINED;

    int32_t resolution = 0;
    if (JS_ToInt32(context, &resolution, argv[0]) != 0) return JS_UNDEFINED;
    resolution = std::max(0, resolution);

    std::vector<float> zeros(static_cast<size_t>(resolution), 0.0f);
    JSValue            object  = JS_NewObject(context);
    JSValue            left    = CreateJSFloat32Array(context, zeros);
    JSValue            right   = CreateJSFloat32Array(context, zeros);
    JSValue            average = CreateJSFloat32Array(context, zeros);

    if (JS_IsException(left) || JS_IsException(right) || JS_IsException(average)) {
        JS_FreeValue(context, object);
        JS_FreeValue(context, left);
        JS_FreeValue(context, right);
        JS_FreeValue(context, average);
        return JS_EXCEPTION;
    }

    JS_SetPropertyStr(context, object, "left", JS_DupValue(context, left));
    JS_SetPropertyStr(context, object, "right", JS_DupValue(context, right));
    JS_SetPropertyStr(context, object, "average", JS_DupValue(context, average));

    opaque->audio_buffers.push_back(AudioBufferBinding {
        .resolution = static_cast<uint32_t>(resolution),
        .object     = JS_DupValue(context, object),
        .left       = JS_DupValue(context, left),
        .right      = JS_DupValue(context, right),
        .average    = JS_DupValue(context, average),
    });
    UpdateAudioBufferBindings(opaque);

    JS_FreeValue(context, left);
    JS_FreeValue(context, right);
    JS_FreeValue(context, average);
    return object;
}

JSValue NativeHasVideoTexture(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_FALSE;

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_FALSE;

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr) return JS_FALSE;
    return JS_NewBool(context, ! ResolveVideoTextureKeysForNode(opaque, node).empty());
}

JSValue NativeVideoTextureCall(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || opaque->scene == nullptr || argc < 2) return JS_UNDEFINED;

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_UNDEFINED;

    std::string command;
    if (! ReadJSString(context, argv[1], &command)) return JS_UNDEFINED;

    const bool command_requires_video_decoder =
        command == "play" || command == "pause" || command == "stop" ||
        command == "setCurrentTime";
    if (command_requires_video_decoder &&
        opaque->scene->deferredRuntimeImageLayerIds.count(node_id) != 0) {
        // Some Wallpaper Engine intro layers intentionally start hidden, then call
        // getVideoTexture().stop()/setCurrentTime() during init before their first visible=true
        // update tick. Visibility-driven materialization is too late for those scripts: the
        // placeholder has no mesh/material, so the video command would resolve to an empty no-op
        // controller. Materialize on the first concrete video command instead, keeping
        // getVideoTexture() itself cheap while preserving hidden one-shot video setup.
        if (! MaterializeDeferredImageLayerIfNeeded(opaque, node_id)) {
            LOG_ERROR("SceneVideoTextureCall: failed to materialize deferred layer=%d command='%s'",
                      node_id,
                      command.c_str());
            return JS_UNDEFINED;
        }
        opaque->scene->ApplyLayerVisibility(node_id);
    }

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr) {
        return command == "isPlaying" ? JS_NewBool(context, false) : JS_UNDEFINED;
    }

    const auto keys = ResolveVideoTextureKeysForNode(opaque, node);
    if (keys.empty()) {
        if (command == "isPlaying") return JS_NewBool(context, false);
        if (command == "getCurrentTime") return JS_NewFloat64(context, 0.0);
        if (command == "rate") return argc >= 3 ? JS_UNDEFINED : JS_NewFloat64(context, 1.0);
        return JS_UNDEFINED;
    }

    auto current_time_for_key = [&](const std::string& key) {
        double value = 0.0;
        if (const auto value_it = opaque->scene->videoTextureCurrentTimes.find(key);
            value_it != opaque->scene->videoTextureCurrentTimes.end()) {
            value = value_it->second;
        }

        const auto paused_it = opaque->scene->videoTexturePaused.find(key);
        const bool paused =
            paused_it != opaque->scene->videoTexturePaused.end() ? paused_it->second : false;
        const bool running =
            opaque->scene->videoTextureStopped.count(key) == 0 && ! paused;
        if (! running) return std::max(0.0, value);

        const auto anchor_it = opaque->scene->videoTextureCurrentTimeRuntimeAnchors.find(key);
        if (anchor_it == opaque->scene->videoTextureCurrentTimeRuntimeAnchors.end()) {
            return std::max(0.0, value);
        }

        const auto rate_it = opaque->scene->videoTextureRates.find(key);
        const double rate =
            rate_it != opaque->scene->videoTextureRates.end() ? rate_it->second : 1.0;
        return std::max(0.0, value + (opaque->runtime_seconds - anchor_it->second) * rate);
    };

    if (command == "play" || command == "pause" || command == "stop") {
        for (const auto& key : keys) {
            const double current_seconds = current_time_for_key(key);
            if (command == "stop") {
                // Wallpaper Engine stop() is a terminal decoder command, not just pause().
                // Finished intro layers rely on this to release playback work after they fade out.
                opaque->scene->videoTexturePaused[key] = true;
                opaque->scene->videoTextureStopped.insert(key);
                opaque->scene->videoTextureCurrentTimes[key] = 0.0;
                opaque->scene->videoTextureCurrentTimeRuntimeAnchors[key] =
                    opaque->runtime_seconds;
            } else {
                const bool paused = command == "pause";
                const auto current_it = opaque->scene->videoTexturePaused.find(key);
                if (current_it != opaque->scene->videoTexturePaused.end() &&
                    current_it->second == paused &&
                    opaque->scene->videoTextureStopped.count(key) == 0) {
                    continue;
                }
                opaque->scene->videoTextureStopped.erase(key);
                opaque->scene->videoTexturePaused[key] = paused;
                opaque->scene->videoTextureCurrentTimes[key] = current_seconds;
                opaque->scene->videoTextureCurrentTimeRuntimeAnchors[key] =
                    opaque->runtime_seconds;
            }
        }
        return JS_UNDEFINED;
    }

    if (command == "setCurrentTime") {
        double seconds = 0.0;
        if (argc < 3 || JS_ToFloat64(context, &seconds, argv[2]) != 0 ||
            ! std::isfinite(seconds)) {
            LOG_ERROR("SceneVideoTextureSeekRequest: layer=%d invalid setCurrentTime argument",
                      node_id);
            return JS_UNDEFINED;
        }

        // Video textures may not have a Vulkan cache entry while init scripts are running. Keep the
        // latest requested timestamp on Scene so the render thread can apply it after the pass that
        // owns the concrete decoder has been prepared.
        const double clamped_seconds = std::max(0.0, seconds);
        for (const auto& key : keys) {
            opaque->scene->videoTextureSeekRequests[key] = clamped_seconds;
            opaque->scene->videoTextureCurrentTimes[key]  = clamped_seconds;
            opaque->scene->videoTextureCurrentTimeRuntimeAnchors[key] = opaque->runtime_seconds;
            LOG_VERBOSE("SceneVideoTextureSeekRequest: layer=%d texture='%s' seconds=%.3f",
                        node_id,
                        key.c_str(),
                        clamped_seconds);
        }
        return JS_UNDEFINED;
    }

    if (command == "getCurrentTime") {
        double current_seconds = 0.0;
        for (const auto& key : keys) {
            current_seconds = std::max(current_seconds, current_time_for_key(key));
        }
        return JS_NewFloat64(context, current_seconds);
    }

    if (command == "rate") {
        if (argc >= 3) {
            double rate = 1.0;
            if (JS_ToFloat64(context, &rate, argv[2]) != 0 || ! std::isfinite(rate)) {
                return JS_UNDEFINED;
            }

            rate = std::max(0.0, rate);
            for (const auto& key : keys) {
                opaque->scene->videoTextureCurrentTimes[key] = current_time_for_key(key);
                opaque->scene->videoTextureCurrentTimeRuntimeAnchors[key] =
                    opaque->runtime_seconds;
                opaque->scene->videoTextureRates[key] = rate;
            }
            return JS_UNDEFINED;
        }

        const auto rate_it = opaque->scene->videoTextureRates.find(keys.front());
        return JS_NewFloat64(context,
                             rate_it != opaque->scene->videoTextureRates.end() ? rate_it->second
                                                                               : 1.0);
    }

    if (command == "isPlaying") {
        bool any_playing = false;
        for (const auto& key : keys) {
            if (opaque->scene->videoTextureStopped.count(key) != 0) continue;
            const auto paused_it = opaque->scene->videoTexturePaused.find(key);
            const bool paused =
                paused_it != opaque->scene->videoTexturePaused.end() ? paused_it->second : false;
            any_playing = any_playing || ! paused;
        }
        return JS_NewBool(context, any_playing);
    }

    return JS_UNDEFINED;
}

JSValue NativeHasTextureAnimation(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_FALSE;

    int32_t node_id = 0;
    int32_t slot    = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0 || JS_ToInt32(context, &slot, argv[1]) != 0) {
        return JS_FALSE;
    }

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr) return JS_FALSE;
    return JS_NewBool(
        context, ResolveTextureAnimationNode(opaque, node, static_cast<usize>(slot)) != nullptr);
}

JSValue NativeIsDeferredRuntimeLayer(JSContext* context, JSValueConst, int argc,
                                     JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_FALSE;

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_FALSE;

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr) return JS_FALSE;

    int32_t layer_id = FindOwningLayerId(opaque, node);
    if (layer_id == 0) {
        // Layer proxies pass their logical layer id as the node id. A deferred placeholder may be
        // the only scene node for that layer, so the bridge must fall back to the proxy id when no
        // owner record is available yet.
        layer_id = node_id;
    }

    return JS_NewBool(context, IsDeferredRuntimeLayer(opaque, layer_id));
}

JSValue NativeGetAnimationLayerCount(JSContext* context, JSValueConst, int argc,
                                     JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 1) return JS_NewInt32(context, 0);

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_NewInt32(context, 0);

    auto*       node = ResolvePuppetLayerNode(opaque, FindNodeById(opaque, node_id));
    const auto* data = GetNodeData(opaque, node);
    if (data == nullptr || ! data->puppet_layer.hasPuppet()) return JS_NewInt32(context, 0);
    return JS_NewInt32(context, static_cast<int32_t>(data->puppet_layer.AnimationLayerCount()));
}

JSValue NativeResolveAnimationLayer(JSContext* context, JSValueConst, int argc,
                                    JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_NewInt32(context, -1);

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_NewInt32(context, -1);

    auto* node = ResolvePuppetLayerNode(opaque, FindNodeById(opaque, node_id));
    if (node == nullptr) return JS_NewInt32(context, -1);

    int32_t index = 0;
    if (JS_ToInt32(context, &index, argv[1]) == 0) {
        return JS_NewInt32(context, index);
    }

    std::string name;
    if (! ReadJSString(context, argv[1], &name)) return JS_NewInt32(context, -1);

    const auto* data = GetNodeData(opaque, node);
    if (data == nullptr || ! data->puppet_layer.hasPuppet()) return JS_NewInt32(context, -1);

    for (usize i = 0; i < data->puppet_layer.AnimationLayerCount(); i++) {
        if (const auto* anim = data->puppet_layer.AnimationDefinition(i);
            anim != nullptr && anim->name == name) {
            return JS_NewInt32(context, static_cast<int32_t>(i));
        }
    }

    return JS_NewInt32(context, -1);
}

JSValue NativeHasAnimationLayer(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_FALSE;

    int32_t node_id = 0;
    int32_t index   = -1;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0 || JS_ToInt32(context, &index, argv[1]) != 0) {
        return JS_FALSE;
    }

    auto* node = ResolvePuppetLayerNode(opaque, FindNodeById(opaque, node_id));
    return JS_NewBool(
        context,
        FindAnimationLayerState(opaque, node, static_cast<usize>(std::max(index, 0))) != nullptr);
}

JSValue NativeHasAnimationLayerMember(JSContext* context, JSValueConst, int argc,
                                      JSValueConst* argv) {
    if (argc < 3) return JS_FALSE;

    std::string member_name;
    if (! ReadJSString(context, argv[2], &member_name)) return JS_FALSE;
    if (member_name == "play" || member_name == "stop" || member_name == "pause" ||
        member_name == "isPlaying" || member_name == "getFrame" || member_name == "setFrame" ||
        member_name == "addEndedCallback") {
        return JS_TRUE;
    }

    return JS_NewBool(context,
                      AnimationLayerValueType(member_name).supported || member_name == "fps" ||
                          member_name == "frameCount" || member_name == "duration" ||
                          member_name == "name");
}

JSValue NativeGetAnimationLayerProperty(JSContext* context, JSValueConst, int argc,
                                        JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 3) return JS_UNDEFINED;

    int32_t node_id = 0;
    int32_t index   = -1;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0 || JS_ToInt32(context, &index, argv[1]) != 0) {
        return JS_UNDEFINED;
    }

    std::string property_name;
    if (! ReadJSString(context, argv[2], &property_name)) return JS_UNDEFINED;

    auto* node = ResolvePuppetLayerNode(opaque, FindNodeById(opaque, node_id));
    if (node == nullptr || index < 0) return JS_UNDEFINED;

    if (property_name == "fps" || property_name == "frameCount" || property_name == "duration" ||
        property_name == "name") {
        const auto* animation = FindAnimationDefinition(opaque, node, static_cast<usize>(index));
        if (animation == nullptr) return JS_UNDEFINED;
        if (property_name == "fps") return JS_NewFloat64(context, animation->fps);
        if (property_name == "frameCount") return JS_NewInt32(context, animation->length);
        if (property_name == "duration") return JS_NewFloat64(context, animation->max_time);
        if (property_name == "name")
            return JS_NewStringLen(context, animation->name.c_str(), animation->name.size());
    }

    const auto value =
        ReadAnimationLayerPropertyValue(opaque, node, static_cast<usize>(index), property_name);
    if (! value.has_value()) return JS_UNDEFINED;
    const auto script_value = value->toScriptValue();
    return script_value.has_value() ? ScriptValueToJS(context, *script_value) : JS_UNDEFINED;
}

JSValue NativeSetAnimationLayerProperty(JSContext* context, JSValueConst, int argc,
                                        JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 4) return JS_FALSE;

    int32_t node_id = 0;
    int32_t index   = -1;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0 || JS_ToInt32(context, &index, argv[1]) != 0) {
        return JS_FALSE;
    }

    std::string property_name;
    if (! ReadJSString(context, argv[2], &property_name)) return JS_FALSE;

    auto* node = ResolvePuppetLayerNode(opaque, FindNodeById(opaque, node_id));
    if (node == nullptr || index < 0) return JS_FALSE;

    const auto hint = AnimationLayerValueType(property_name);
    if (! hint.supported) return JS_FALSE;

    const auto value = ReadDynamicValueFromJS(context, argv[3], hint.type);
    if (! value.has_value()) return JS_FALSE;

    return JS_NewBool(context,
                      ApplyAnimationLayerPropertyValue(
                          opaque, node, static_cast<usize>(index), property_name, *value));
}

JSValue NativeAnimationLayerCall(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 3) return JS_UNDEFINED;

    int32_t node_id = 0;
    int32_t index   = -1;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0 || JS_ToInt32(context, &index, argv[1]) != 0) {
        return JS_UNDEFINED;
    }

    std::string command;
    if (! ReadJSString(context, argv[2], &command)) return JS_UNDEFINED;

    auto*      node         = FindNodeById(opaque, node_id);
    const auto target_nodes = CollectPuppetLayerNodes(opaque, node);
    if (target_nodes.empty() || index < 0) return JS_UNDEFINED;

    const auto find_target_state = [&](SceneNode* target_node)
        -> std::pair<WPPuppetLayer::AnimationLayer*, const WPPuppet::Animation*> {
        auto* data = GetNodeData(opaque, target_node);
        if (data == nullptr || ! data->puppet_layer.hasPuppet()) return { nullptr, nullptr };
        auto* target_layer = data->puppet_layer.AnimationLayerState(static_cast<usize>(index));
        const auto* target_animation =
            data->puppet_layer.AnimationDefinition(static_cast<usize>(index));
        return { target_layer, target_animation };
    };

    WPPuppetLayer::AnimationLayer* layer     = nullptr;
    const WPPuppet::Animation*     animation = nullptr;
    for (auto* target_node : target_nodes) {
        auto [candidate_layer, candidate_animation] = find_target_state(target_node);
        if (candidate_layer == nullptr || candidate_animation == nullptr) continue;
        layer     = candidate_layer;
        animation = candidate_animation;
        break;
    }
    if (layer == nullptr || animation == nullptr) return JS_UNDEFINED;

    if (command == "play") {
        // Animation-layer script methods share the same duplicated-puppet problem as property
        // writes: effect-backed image layers keep a logical puppet plus one or more rendered effect
        // pass puppets. Broadcast playback commands so init/update scripts cannot restart only the
        // invisible logical copy while the visible effect copy keeps advancing.
        std::size_t applied = 0;
        for (auto* target_node : target_nodes) {
            auto [target_layer, target_animation] = find_target_state(target_node);
            if (target_layer == nullptr || target_animation == nullptr) continue;
            // Restart single-shot layers from their authored boundary every time play() is called
            // so repeated clicks reliably replay the animation instead of resuming a stale end
            // pose.
            if (target_animation->mode == WPPuppet::PlayMode::Single) {
                target_layer->cur_time =
                    target_layer->rate < 0.0 ? target_animation->EndTime() : 0.0;
            }
            target_layer->pending_ended_callback = false;
            target_layer->playing                = true;
            applied++;
        }
        LOG_INFO("SceneAnimationLayerCall: layer=%d animation-index=%zu command='play' targets=%zu "
                 "applied=%zu",
                 node_id,
                 static_cast<size_t>(index),
                 target_nodes.size(),
                 applied);
        return JS_UNDEFINED;
    }
    if (command == "stop") {
        // Stop has to reset every puppet copy, otherwise the visible effect pass can keep its old
        // animation time while the script-facing logical layer reports that playback has stopped.
        std::size_t applied = 0;
        for (auto* target_node : target_nodes) {
            auto [target_layer, target_animation] = find_target_state(target_node);
            if (target_layer == nullptr || target_animation == nullptr) continue;
            target_layer->pending_ended_callback = false;
            target_layer->playing                = false;
            target_layer->cur_time               = 0.0;
            applied++;
        }
        LOG_INFO("SceneAnimationLayerCall: layer=%d animation-index=%zu command='stop' targets=%zu "
                 "applied=%zu",
                 node_id,
                 static_cast<size_t>(index),
                 target_nodes.size(),
                 applied);
        return JS_UNDEFINED;
    }
    if (command == "pause") {
        // Pause is also broadcast because Wallpaper Engine exposes animation layers as one object
        // even when the renderer has split that layer into several runtime puppet instances.
        std::size_t applied = 0;
        for (auto* target_node : target_nodes) {
            auto [target_layer, target_animation] = find_target_state(target_node);
            if (target_layer == nullptr || target_animation == nullptr) continue;
            target_layer->pending_ended_callback = false;
            target_layer->playing                = false;
            applied++;
        }
        LOG_INFO("SceneAnimationLayerCall: layer=%d animation-index=%zu command='pause' "
                 "targets=%zu applied=%zu",
                 node_id,
                 static_cast<size_t>(index),
                 target_nodes.size(),
                 applied);
        return JS_UNDEFINED;
    }
    if (command == "isPlaying") {
        return JS_NewBool(context, layer->playing);
    }
    if (command == "getFrame") {
        const auto frame = animation->getInterpolationInfo(&layer->cur_time).frame_a;
        return JS_NewInt32(context, static_cast<int32_t>(frame));
    }
    if (command == "setFrame") {
        if (argc < 4) return JS_UNDEFINED;
        int32_t frame = 0;
        if (JS_ToInt32(context, &frame, argv[3]) == 0) {
            // Frame seeks are a visible pose write, so every duplicate puppet copy must land on the
            // same authored frame before the next render tick samples its skinning matrices.
            std::size_t applied = 0;
            for (auto* target_node : target_nodes) {
                auto [target_layer, target_animation] = find_target_state(target_node);
                if (target_layer == nullptr || target_animation == nullptr) continue;
                const auto clamped_frame =
                    std::clamp(frame, 0, std::max(target_animation->length - 1, 0));
                target_layer->cur_time =
                    target_animation->frame_time * static_cast<double>(clamped_frame);
                applied++;
            }
            LOG_INFO("SceneAnimationLayerCall: layer=%d animation-index=%zu command='setFrame' "
                     "frame=%d targets=%zu applied=%zu",
                     node_id,
                     static_cast<size_t>(index),
                     frame,
                     target_nodes.size(),
                     applied);
        }
        return JS_UNDEFINED;
    }

    return JS_UNDEFINED;
}

JSValue NativeAddAnimationLayerEndedCallback(JSContext* context, JSValueConst, int argc,
                                             JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 3 || ! JS_IsFunction(context, argv[2])) return JS_FALSE;

    int32_t node_id = 0;
    int32_t index   = -1;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0 || JS_ToInt32(context, &index, argv[1]) != 0) {
        return JS_FALSE;
    }

    auto* node = ResolvePuppetLayerNode(opaque, FindNodeById(opaque, node_id));
    if (node == nullptr || index < 0) return JS_FALSE;

    auto& state = opaque->animation_layer_states[node][static_cast<usize>(index)];
    state.ended_callbacks.push_back(JS_DupValue(context, argv[2]));
    return JS_TRUE;
}

JSValue NativeTextureAnimationGet(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 3) return JS_UNDEFINED;

    int32_t node_id = 0;
    int32_t slot    = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0 || JS_ToInt32(context, &slot, argv[1]) != 0) {
        return JS_UNDEFINED;
    }

    std::string command;
    if (! ReadJSString(context, argv[2], &command)) return JS_UNDEFINED;

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr) return JS_UNDEFINED;

    node = ResolveTextureAnimationNode(opaque, node, static_cast<usize>(slot));
    if (node == nullptr) return JS_UNDEFINED;

    auto states_it = opaque->texture_states.find(node);
    if (states_it == opaque->texture_states.end()) return JS_UNDEFINED;

    auto state_it = states_it->second.find(static_cast<usize>(slot));
    if (state_it == states_it->second.end()) return JS_UNDEFINED;

    auto& state = state_it->second;
    if (command == "frameCount")
        return JS_NewInt32(context, static_cast<int32_t>(state.animation.numFrames()));
    if (command == "duration") return JS_NewFloat64(context, state.animation.Duration());
    if (command == "rate") return JS_NewFloat64(context, state.rate);
    return JS_UNDEFINED;
}

JSValue NativeTextureAnimationSet(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 4) return JS_FALSE;

    int32_t node_id = 0;
    int32_t slot    = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0 || JS_ToInt32(context, &slot, argv[1]) != 0) {
        return JS_FALSE;
    }

    std::string command;
    if (! ReadJSString(context, argv[2], &command)) return JS_FALSE;

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr) return JS_FALSE;

    node = ResolveTextureAnimationNode(opaque, node, static_cast<usize>(slot));
    if (node == nullptr) return JS_FALSE;

    auto states_it = opaque->texture_states.find(node);
    if (states_it == opaque->texture_states.end()) return JS_FALSE;

    auto state_it = states_it->second.find(static_cast<usize>(slot));
    if (state_it == states_it->second.end()) return JS_FALSE;

    auto& state = state_it->second;
    if (command == "rate") {
        double rate = 1.0;
        if (JS_ToFloat64(context, &rate, argv[3]) != 0) return JS_FALSE;
        state.rate = std::max(0.0, rate);
        return JS_TRUE;
    }

    return JS_FALSE;
}

JSValue NativeTextureAnimationCall(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 3) return JS_UNDEFINED;

    int32_t node_id = 0;
    int32_t slot    = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0 || JS_ToInt32(context, &slot, argv[1]) != 0) {
        return JS_UNDEFINED;
    }

    std::string command;
    if (! ReadJSString(context, argv[2], &command)) return JS_UNDEFINED;

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr) return JS_UNDEFINED;

    node = ResolveTextureAnimationNode(opaque, node, static_cast<usize>(slot));
    if (node == nullptr) return JS_UNDEFINED;

    auto states_it = opaque->texture_states.find(node);
    if (states_it == opaque->texture_states.end()) return JS_UNDEFINED;

    auto state_it = states_it->second.find(static_cast<usize>(slot));
    if (state_it == states_it->second.end()) return JS_UNDEFINED;

    auto& state = state_it->second;
    if (command == "play") {
        state.animation.Play();
        return JS_UNDEFINED;
    }
    if (command == "stop") {
        state.animation.Stop();
        return JS_UNDEFINED;
    }
    if (command == "pause") {
        state.animation.Pause();
        return JS_UNDEFINED;
    }
    if (command == "isPlaying") {
        return JS_NewBool(context, state.animation.IsPlaying());
    }
    if (command == "getFrame") {
        return JS_NewInt32(context, static_cast<int32_t>(state.animation.CurrentFrameIndex()));
    }
    if (command == "setFrame") {
        if (argc < 4) return JS_UNDEFINED;
        int32_t frame = 0;
        if (JS_ToInt32(context, &frame, argv[3]) == 0) {
            state.animation.SetCurrentFrame(frame);
        }
        return JS_UNDEFINED;
    }
    if (command == "join") {
        state.animation = state.base_animation;
        state.rate      = 1.0;
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

JSValue NativeRotateLayerObjectSpace(JSContext* context, JSValueConst, int argc,
                                     JSValueConst* argv) {
    auto* opaque = GetOpaque(context);
    if (opaque == nullptr || argc < 2) return JS_FALSE;

    int32_t node_id = 0;
    if (JS_ToInt32(context, &node_id, argv[0]) != 0) return JS_FALSE;

    auto* node = FindNodeById(opaque, node_id);
    if (node == nullptr) return JS_FALSE;

    const auto angles = ReadDynamicValueFromJS(context, argv[1], WPDynamicValue::Type::Float3);
    if (! angles.has_value()) return JS_FALSE;

    // rotateObjectSpace is called from authored SceneScript, where angle deltas are degrees. The
    // SceneNode rotation accumulator is radian-based, so convert before adding the delta.
    const auto runtime_angles = ConvertFloat3Scale(*angles, kSceneScriptDegreesToRadians);
    std::array<float, 3> delta {};
    if (! runtime_angles.tryGet(&delta)) return JS_FALSE;

    const auto& current = node->Rotation();
    node->SetRotation(
        Eigen::Vector3f { current.x() + delta[0], current.y() + delta[1], current.z() + delta[2] });
    return JS_TRUE;
}

void SetJSProperty(JSContext* context, JSValueConst object, const char* name,
                   const WPScriptValue& value) {
    JS_SetPropertyStr(context, object, name, ScriptValueToJS(context, value));
}

void UpdateJSNumericArray(JSContext* context, JSValueConst target,
                          const std::vector<float>& values) {
    for (uint32_t index = 0; index < values.size(); index++) {
        JS_SetPropertyUint32(
            context, target, index, JS_NewFloat64(context, values[static_cast<size_t>(index)]));
    }
}

JSValue CreateJSFloat32Array(JSContext* context, const std::vector<float>& values) {
    JSValue length_arg = JS_NewUint32(context, static_cast<uint32_t>(values.size()));
    JSValue array      = JS_NewTypedArray(context, 1, &length_arg, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(context, length_arg);
    if (JS_IsException(array)) return array;

    if (! values.empty() && ! UpdateJSFloat32Array(context, array, values)) {
        JS_FreeValue(context, array);
        return JS_EXCEPTION;
    }
    return array;
}

bool UpdateJSFloat32Array(JSContext* context, JSValueConst target,
                          const std::vector<float>& values) {
    if (JS_GetTypedArrayType(target) != JS_TYPED_ARRAY_FLOAT32) return false;

    size_t  byte_offset       = 0;
    size_t  byte_length       = 0;
    size_t  bytes_per_element = 0;
    JSValue buffer =
        JS_GetTypedArrayBuffer(context, target, &byte_offset, &byte_length, &bytes_per_element);
    if (JS_IsException(buffer)) return false;

    const size_t expected_bytes = values.size() * sizeof(float);
    if (byte_length != expected_bytes || bytes_per_element != sizeof(float)) {
        JS_FreeValue(context, buffer);
        return false;
    }

    size_t   buffer_size = 0;
    uint8_t* raw_buffer  = JS_GetArrayBuffer(context, &buffer_size, buffer);
    if (raw_buffer == nullptr || byte_offset + expected_bytes > buffer_size) {
        JS_FreeValue(context, buffer);
        return false;
    }

    auto* dest = reinterpret_cast<float*>(raw_buffer + byte_offset);
    std::copy(values.begin(), values.end(), dest);
    JS_FreeValue(context, buffer);
    return true;
}

void UpdateAudioBufferBindings(WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr || opaque->runtime.context == nullptr ||
        (opaque->external_audio_samples.empty() &&
         (opaque->scene == nullptr || opaque->scene->soundManager == nullptr))) {
        return;
    }

    JSContext* context = opaque->runtime.context;
    for (auto& binding : opaque->audio_buffers) {
        std::vector<float> left;
        std::vector<float> right;
        std::vector<float> average;
        if (! GetExternalAudioBufferValues(opaque, binding.resolution, &left, &right, &average)) {
            opaque->scene->soundManager->GetSpectrum(binding.resolution, &left, &right, &average);
        }

        UpdateJSFloat32Array(context, binding.left, left);
        UpdateJSFloat32Array(context, binding.right, right);
        UpdateJSFloat32Array(context, binding.average, average);
    }
}

bool ApplyPropertyAnimationInstance(WPSceneScriptHost::Opaque* opaque,
                                    PropertyAnimationInstance& animation) {
    if (opaque == nullptr || animation.registration.animation == nullptr) return false;

    const auto raw_value = EvaluatePropertyAnimation(*animation.registration.animation,
                                                     animation.state,
                                                     animation.registration.base_value,
                                                     animation.registration.value_type);
    if (! raw_value.has_value()) return false;

    const auto value = ClampOpacityRegistrationValue(animation.registration, *raw_value);
    return ApplyRegistrationValue(opaque, animation.registration, value);
}

void UpdatePropertyAnimations(WPSceneScriptHost::Opaque* opaque, double frame_time) {
    if (opaque == nullptr) return;

    for (auto& animation : opaque->property_animations) {
        if (animation.registration.animation == nullptr) continue;
        AdvancePropertyAnimationState(
            *animation.registration.animation, animation.state, frame_time);
        ApplyPropertyAnimationInstance(opaque, animation);
    }
}

CursorPositionState ComputeCursorPositionState(const WPSceneScriptHost::Opaque* opaque) {
    CursorPositionState state;
    if (opaque == nullptr || opaque->scene == nullptr) return state;

    const float normalized_x = std::clamp(opaque->scene->mousePositionNormalized[0], 0.0f, 1.0f);
    const float normalized_y = std::clamp(opaque->scene->mousePositionNormalized[1], 0.0f, 1.0f);
    const float screen_x     = normalized_x * static_cast<float>(opaque->scene->ortho[0]);
    const float screen_y     = normalized_y * static_cast<float>(opaque->scene->ortho[1]);

    double             world_x          = screen_x;
    double             world_y          = static_cast<float>(opaque->scene->ortho[1]) - screen_y;
    const SceneCamera* camera           = opaque->scene->activeCamera;
    const auto         global_camera_it = opaque->scene->cameras.find("global");
    if (global_camera_it != opaque->scene->cameras.end() && global_camera_it->second) {
        camera = global_camera_it->second.get();
    }
    if (camera != nullptr) {
        const Eigen::Vector3d camera_pos = camera->GetPosition();
        const double          left       = camera_pos.x() - camera->Width() / 2.0;
        const double          top        = camera_pos.y() + camera->Height() / 2.0;
        world_x                          = left + normalized_x * camera->Width();
        world_y                          = top - normalized_y * camera->Height();
    }

    state.screen_x = screen_x;
    state.screen_y = screen_y;
    state.world_x  = world_x;
    state.world_y  = world_y;
    return state;
}

void UpdateInputState(WPSceneScriptHost::Opaque* opaque) {
    if (opaque == nullptr || opaque->scene == nullptr || opaque->runtime.context == nullptr ||
        JS_IsUndefined(opaque->input)) {
        return;
    }

    JSContext* context = opaque->runtime.context;
    const auto cursor  = ComputeCursorPositionState(opaque);

    auto set_vec2 = [context](JSValueConst object, double x, double y) {
        JS_SetPropertyStr(context, object, "x", JS_NewFloat64(context, x));
        JS_SetPropertyStr(context, object, "y", JS_NewFloat64(context, y));
    };
    auto set_vec3 = [context](JSValueConst object, double x, double y, double z) {
        JS_SetPropertyStr(context, object, "x", JS_NewFloat64(context, x));
        JS_SetPropertyStr(context, object, "y", JS_NewFloat64(context, y));
        JS_SetPropertyStr(context, object, "z", JS_NewFloat64(context, z));
    };

    JSValue cursor_world  = JS_GetPropertyStr(context, opaque->input, "cursorWorldPosition");
    JSValue cursor_screen = JS_GetPropertyStr(context, opaque->input, "cursorScreenPosition");
    set_vec3(cursor_world, cursor.world_x, cursor.world_y, 0.0);
    set_vec2(cursor_screen, cursor.screen_x, cursor.screen_y);
    JS_SetPropertyStr(context,
                      opaque->input,
                      "cursorLeftDown",
                      JS_NewBool(context, opaque->scene->cursorLeftDown));
    JS_FreeValue(context, cursor_world);
    JS_FreeValue(context, cursor_screen);
}

bool AreUserPropertiesEqual(const UserProperty& lhs, const UserProperty& rhs) {
    return lhs.condition == rhs.condition && lhs.is_boolean == rhs.is_boolean &&
           AreUserPropertyValuesEqual(lhs.value, rhs.value);
}

void UpdateGeneralSettingsObject(JSContext* context, JSValueConst target,
                                 const std::unordered_map<std::string, std::string>& settings,
                                 std::unordered_set<std::string>&                    known_names) {
    std::vector<std::string> removed_names;
    removed_names.reserve(known_names.size());
    for (const auto& name : known_names) {
        if (settings.count(name) == 0) removed_names.push_back(name);
    }
    for (const auto& name : removed_names) {
        JSAtom atom = JS_NewAtom(context, name.c_str());
        JS_DeleteProperty(context, target, atom, 0);
        JS_FreeAtom(context, atom);
        known_names.erase(name);
    }

    for (const auto& [name, value] : settings) {
        JS_SetPropertyStr(context, target, name.c_str(), JS_NewString(context, value.c_str()));
        known_names.insert(name);
    }
}

void UpdateUserPropertyObject(JSContext* context, JSValueConst target,
                              const UserPropertyMap&           properties,
                              std::unordered_set<std::string>& known_names) {
    std::vector<std::string> removed_names;
    removed_names.reserve(known_names.size());
    for (const auto& name : known_names) {
        if (properties.count(name) == 0) removed_names.push_back(name);
    }
    for (const auto& name : removed_names) {
        JSAtom atom = JS_NewAtom(context, name.c_str());
        JS_DeleteProperty(context, target, atom, 0);
        JS_FreeAtom(context, atom);
        known_names.erase(name);
    }

    for (const auto& [name, property] : properties) {
        const auto script_value = UserPropertyToScriptValue(property.value);
        if (! script_value.has_value()) continue;
        SetJSProperty(context, target, name.c_str(), *script_value);
        known_names.insert(name);
    }
}

JSValue MakeChangedUserProperties(JSContext* context, const UserPropertyMap& current,
                                  const UserPropertyMap* previous) {
    JSValue object = JS_NewObject(context);
    for (const auto& [name, property] : current) {
        bool changed = previous == nullptr;
        if (! changed) {
            const auto previous_it = previous->find(name);
            changed                = previous_it == previous->end() ||
                                     ! AreUserPropertiesEqual(property, previous_it->second);
        }
        if (! changed) continue;

        const auto script_value = UserPropertyToScriptValue(property.value);
        if (! script_value.has_value()) continue;
        SetJSProperty(context, object, name.c_str(), *script_value);
    }
    return object;
}

JSValue MakeChangedGeneralSettings(JSContext*                                          context,
                                   const std::unordered_map<std::string, std::string>& current,
                                   const std::unordered_map<std::string, std::string>* previous) {
    JSValue object = JS_NewObject(context);
    for (const auto& [name, value] : current) {
        bool changed = previous == nullptr;
        if (! changed) {
            const auto previous_it = previous->find(name);
            changed                = previous_it == previous->end() || previous_it->second != value;
        }
        if (! changed) continue;
        JS_SetPropertyStr(context, object, name.c_str(), JS_NewString(context, value.c_str()));
    }
    return object;
}

nlohmann::json* ResolveLocalStorageBucket(WPSceneScriptHost::Opaque* opaque,
                                          LocalStorageLocation       location) {
    if (opaque == nullptr) return nullptr;
    return location == LocalStorageLocation::Global ? &opaque->local_storage_global
                                                    : &opaque->local_storage_screen;
}

bool ShouldKeepScriptAuthoredInitialValue(const WPSceneScriptRegistration& registration) {
    if (registration.target_kind != WPSceneScriptTargetKind::Layer) return false;

    constexpr std::array<std::string_view, 3> kAuthoredBaseLayerProperties {
        "visible",
        "origin",
        "scale",
    };

    // Wallpaper Engine gives property scripts their authored/user-resolved base value during
    // init(), even when a parser-time probe has already produced a derived live layer value.
    // Transform scripts commonly keep this init() input as their base for applyUserProperties()
    // formulas, so replacing origin/scale with renderer-resolved values makes later user-property
    // deltas accumulate from the wrong coordinate space.
    return std::any_of(kAuthoredBaseLayerProperties.begin(),
                       kAuthoredBaseLayerProperties.end(),
                       [&](std::string_view property_name) {
                           return registration.property_name == property_name;
                       });
}

bool InitializeScriptInstance(WPSceneScriptHost::Opaque* opaque, ScriptInstance& instance) {
    if (opaque == nullptr || opaque->runtime.context == nullptr || opaque->scene == nullptr)
        return false;

    JSContext* context = opaque->runtime.context;
    JSValue    global  = JS_GetGlobalObject(context);

    WPScriptEvaluationContext base_context;
    base_context.canvas_size = {
        static_cast<double>(opaque->scene->ortho[0]),
        static_cast<double>(opaque->scene->ortho[1]),
    };

    if (! opaque->initialized) {
        // During the initial scene bootstrap, Wallpaper Engine exposes the authored property value
        // to init() first and dispatches user-property overrides afterwards. Keeping that order
        // prevents init-time script side effects from seeing inactive conditional branches too
        // early, while ApplyUserProperties() below still installs the user's actual value before
        // the first frame update.
        instance.current_value = instance.registration.setting.value;
    } else if (instance.registration.setting.hasUserBinding()) {
        instance.current_value = EvaluateRegistrationSetting(
            instance.registration, &opaque->user_properties, base_context);
    } else {
        instance.current_value =
            instance.registration.setting.evaluate(&opaque->user_properties, nullptr, base_context);
    }
    const bool keep_script_base_value =
        ShouldKeepScriptAuthoredInitialValue(instance.registration);
    if (! keep_script_base_value) {
        if (const auto animated_value = ReadRegistrationValue(opaque, instance.registration);
            animated_value.has_value()) {
            instance.current_value = *animated_value;
        }
    }

    FreeJSValue(context, instance.script_properties);
    instance.script_properties = JS_NewObject(context);
    for (const auto& [name, setting] : instance.registration.setting.script_properties) {
        // Wallpaper Engine builds scriptProperties from the authored option values before init().
        // User overrides are delivered immediately afterwards through applyUserProperties(); doing
        // that second phase here would let inactive conditional options poison init-time shared
        // state, which is observable in drag scripts that seed a global flag from isMovable.
        const auto script_value = setting->value.toScriptValue();
        if (! script_value.has_value()) continue;
        SetJSProperty(context, instance.script_properties, name.c_str(), *script_value);
    }

    JSValue env = JS_NewObject(context);
    JS_SetPropertyStr(context, env, "native", JS_DupValue(context, opaque->native_bridge));
    JS_SetPropertyStr(context, env, "engineBase", JS_DupValue(context, opaque->engine_base));
    JS_SetPropertyStr(context, env, "shared", JS_DupValue(context, opaque->shared));
    JS_SetPropertyStr(context, env, "console", JS_DupValue(context, opaque->console));
    JS_SetPropertyStr(context, env, "input", JS_DupValue(context, opaque->input));
    JS_SetPropertyStr(
        context, env, "generalSettings", JS_DupValue(context, opaque->general_settings_object));
    JS_SetPropertyStr(context, env, "scene", JS_DupValue(context, opaque->scene_object));
    JS_SetPropertyStr(
        context, env, "scriptProperties", JS_DupValue(context, instance.script_properties));
    JS_SetPropertyStr(context,
                      env,
                      "instanceId",
                      JS_NewInt32(context, static_cast<int32_t>(instance.instance_id)));
    JS_SetPropertyStr(
        context, env, "nodeId", JS_NewInt32(context, instance.registration.object_id));
    JS_SetPropertyStr(context,
                      env,
                      "propertyName",
                      JS_NewStringLen(context,
                                      instance.registration.property_name.data(),
                                      instance.registration.property_name.size()));
    JS_SetPropertyStr(context,
                      env,
                      "objectKind",
                      JS_NewString(context, TargetKindName(instance.registration.target_kind)));
    JS_SetPropertyStr(
        context,
        env,
        "objectIndex",
        JS_NewInt32(context, static_cast<int32_t>(instance.registration.target_index)));

    JS_SetPropertyStr(context, global, "__sceneScriptEnv", JS_DupValue(context, env));

    const std::string wrapped = BuildPersistentScript(instance.registration.setting.script);
    JSValue           factory = JS_Eval(
        context, wrapped.c_str(), wrapped.size(), "<scene-script-factory>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(factory)) {
        LOG_ERROR("QuickJS compile context: %s",
                  DescribeScriptInstance(opaque, instance.instance_id).c_str());
        LogQuickJSException(context, "compile");
        JS_FreeValue(context, factory);
        JS_FreeValue(context, env);
        JS_SetPropertyStr(context, global, "__sceneScriptEnv", JS_UNDEFINED);
        JS_FreeValue(context, global);
        return false;
    }

    JSValue exports = JS_Call(context, factory, JS_UNDEFINED, 1, &env);
    JS_FreeValue(context, factory);
    JS_FreeValue(context, env);
    JS_SetPropertyStr(context, global, "__sceneScriptEnv", JS_UNDEFINED);
    if (JS_IsException(exports)) {
        LogQuickJSException(context, "instantiate");
        JS_FreeValue(context, exports);
        JS_FreeValue(context, global);
        return false;
    }

    FreeJSValue(context, instance.exports);
    FreeJSValue(context, instance.init_fn);
    FreeJSValue(context, instance.update_fn);
    FreeJSValue(context, instance.apply_user_properties_fn);
    FreeJSValue(context, instance.apply_general_settings_fn);
    FreeJSValue(context, instance.cursor_enter_fn);
    FreeJSValue(context, instance.cursor_leave_fn);
    FreeJSValue(context, instance.cursor_move_fn);
    FreeJSValue(context, instance.cursor_down_fn);
    FreeJSValue(context, instance.cursor_up_fn);
    FreeJSValue(context, instance.cursor_click_fn);
    FreeJSValue(context, instance.media_thumbnail_changed_fn);
    FreeJSValue(context, instance.media_properties_changed_fn);
    FreeJSValue(context, instance.media_playback_changed_fn);
    FreeJSValue(context, instance.destroy_fn);
    FreeJSValue(context, instance.resize_screen_fn);

    instance.exports                  = exports;
    instance.init_fn                  = JS_GetPropertyStr(context, exports, "init");
    instance.update_fn                = JS_GetPropertyStr(context, exports, "update");
    instance.apply_user_properties_fn = JS_GetPropertyStr(context, exports, "applyUserProperties");
    instance.apply_general_settings_fn =
        JS_GetPropertyStr(context, exports, "applyGeneralSettings");
    instance.cursor_enter_fn = JS_GetPropertyStr(context, exports, "cursorEnter");
    instance.cursor_leave_fn = JS_GetPropertyStr(context, exports, "cursorLeave");
    instance.cursor_move_fn  = JS_GetPropertyStr(context, exports, "cursorMove");
    instance.cursor_down_fn  = JS_GetPropertyStr(context, exports, "cursorDown");
    instance.cursor_up_fn    = JS_GetPropertyStr(context, exports, "cursorUp");
    instance.cursor_click_fn = JS_GetPropertyStr(context, exports, "cursorClick");
    instance.media_thumbnail_changed_fn =
        JS_GetPropertyStr(context, exports, "mediaThumbnailChanged");
    instance.media_properties_changed_fn =
        JS_GetPropertyStr(context, exports, "mediaPropertiesChanged");
    instance.media_playback_changed_fn =
        JS_GetPropertyStr(context, exports, "mediaPlaybackChanged");
    instance.destroy_fn       = JS_GetPropertyStr(context, exports, "destroy");
    instance.resize_screen_fn = JS_GetPropertyStr(context, exports, "resizeScreen");
    instance.initialized      = false;

    JS_FreeValue(context, global);
    return true;
}

bool RunScriptInstanceInit(WPSceneScriptHost::Opaque* opaque, ScriptInstance& instance) {
    if (opaque == nullptr || JS_IsUndefined(instance.exports) || instance.initialized) return true;

    JSContext* context = opaque->runtime.context;
    if (! JS_IsUndefined(instance.init_fn)) {
        JSValue result       = JS_UNDEFINED;
        instance.initialized = true;
        const auto script_facing_value =
            ToScriptFacingRegistrationValue(instance.registration, instance.current_value);
        if (const auto script_value = script_facing_value.toScriptValue(); script_value.has_value()) {
            JSValue arg = ScriptValueToJS(context, *script_value);
            result      = JS_Call(context, instance.init_fn, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(context, arg);
        } else {
            result = JS_Call(context, instance.init_fn, JS_UNDEFINED, 0, nullptr);
        }
        if (JS_IsException(result)) {
            LogQuickJSException(context, "init");
            // A failed init leaves authored module-scope state only partially assigned. Keep the
            // instance disabled after logging the real init exception so FrameBegin() does not run
            // update() against undefined setup variables and turn one root cause into per-frame
            // SceneScript noise.
            instance.initialized = false;
            JS_FreeValue(context, result);
            return false;
        } else if (! JS_IsUndefined(result)) {
            const auto hint  = RegistrationValueType(instance.registration);
            const auto value = ReadDynamicValueFromJS(
                context, result, hint.supported ? hint.type : instance.current_value.type());
            if (value.has_value()) {
                const auto runtime_value =
                    FromScriptFacingRegistrationValue(instance.registration, *value);
                instance.current_value = runtime_value;
                ApplyRegistrationValue(opaque, instance.registration, runtime_value);
            }
        }
        if (! JS_IsException(result) && JS_IsUndefined(result)) {
            ApplyRegistrationValue(opaque, instance.registration, instance.current_value);
        }
        JS_FreeValue(context, result);
        return true;
    }

    instance.initialized = true;
    ApplyRegistrationValue(opaque, instance.registration, instance.current_value);
    return true;
}

} // namespace

WPSceneScriptHost::WPSceneScriptHost(Scene* scene): m_scene(scene), m_impl(new Opaque()) {
    m_impl->scene           = scene;
    m_impl->runtime.runtime = JS_NewRuntime();
    if (m_impl->runtime.runtime == nullptr) {
        LOG_ERROR("failed to create SceneScript QuickJS runtime");
        return;
    }

    m_impl->runtime.context = JS_NewContext(m_impl->runtime.runtime);
    if (m_impl->runtime.context == nullptr) {
        LOG_ERROR("failed to create SceneScript QuickJS context");
        JS_FreeRuntime(m_impl->runtime.runtime);
        m_impl->runtime.runtime = nullptr;
        return;
    }

    JSContext* context = m_impl->runtime.context;
    JS_SetContextOpaque(context, m_impl);

    m_impl->shared                  = JS_NewObject(context);
    m_impl->engine_base             = JS_NewObject(context);
    m_impl->console                 = JS_NewObject(context);
    m_impl->input                   = JS_NewObject(context);
    m_impl->general_settings_object = JS_NewObject(context);
    m_impl->scene_object            = JS_NewObject(context);
    m_impl->native_bridge           = JS_NewObject(context);
    m_impl->user_properties_object  = JS_NewObject(context);
    m_impl->user_properties = m_scene != nullptr ? m_scene->userProperties : UserPropertyMap {};
    m_impl->dispatched_user_properties  = m_impl->user_properties;
    m_impl->general_settings            = BuildInitialGeneralSettings();
    m_impl->dispatched_general_settings = m_impl->general_settings;

    JS_SetPropertyStr(
        context, m_impl->console, "log", JS_NewCFunction(context, NativeConsoleLog, "log", 1));
    JS_SetPropertyStr(
        context, m_impl->console, "info", JS_NewCFunction(context, NativeConsoleLog, "info", 1));
    JS_SetPropertyStr(
        context, m_impl->console, "warn", JS_NewCFunction(context, NativeConsoleWarn, "warn", 1));
    JS_SetPropertyStr(
        context, m_impl->console, "error", JS_NewCFunction(context, NativeConsoleWarn, "error", 1));

    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "getLayerPropertyById",
                      JS_NewCFunction(context, NativeGetLayerProperty, "getLayerPropertyById", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "setLayerPropertyById",
                      JS_NewCFunction(context, NativeSetLayerProperty, "setLayerPropertyById", 3));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "hasLayerMember",
                      JS_NewCFunction(context, NativeHasLayerMember, "hasLayerMember", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "hasEffect",
                      JS_NewCFunction(context, NativeHasEffect, "hasEffect", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "hasEffectMember",
                      JS_NewCFunction(context, NativeHasEffectMember, "hasEffectMember", 3));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "getEffectProperty",
                      JS_NewCFunction(context, NativeGetEffectProperty, "getEffectProperty", 3));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "setEffectProperty",
                      JS_NewCFunction(context, NativeSetEffectProperty, "setEffectProperty", 4));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "hasEffectMaterial",
                      JS_NewCFunction(context, NativeHasEffectMaterial, "hasEffectMaterial", 3));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "hasEffectMaterialMember",
        JS_NewCFunction(context, NativeHasEffectMaterialMember, "hasEffectMaterialMember", 4));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "getEffectMaterialProperty",
        JS_NewCFunction(context, NativeGetEffectMaterialProperty, "getEffectMaterialProperty", 4));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "setEffectMaterialProperty",
        JS_NewCFunction(context, NativeSetEffectMaterialProperty, "setEffectMaterialProperty", 5));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "getLayerRelation",
                      JS_NewCFunction(context, NativeGetLayerRelation, "getLayerRelation", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "getLayerChildren",
                      JS_NewCFunction(context, NativeGetLayerChildren, "getLayerChildren", 1));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "getSceneProperty",
                      JS_NewCFunction(context, NativeGetSceneProperty, "getSceneProperty", 1));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "setSceneProperty",
                      JS_NewCFunction(context, NativeSetSceneProperty, "setSceneProperty", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "hasSceneMember",
                      JS_NewCFunction(context, NativeHasSceneMember, "hasSceneMember", 1));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "getSceneLayerCount",
                      JS_NewCFunction(context, NativeGetSceneLayerCount, "getSceneLayerCount", 0));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "enumerateSceneLayers",
        JS_NewCFunction(context, NativeEnumerateSceneLayers, "enumerateSceneLayers", 0));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "getSceneLayer",
                      JS_NewCFunction(context, NativeGetSceneLayer, "getSceneLayer", 1));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "destroySceneLayer",
                      JS_NewCFunction(context, NativeDestroySceneLayer, "destroySceneLayer", 1));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "createSceneLayer",
                      JS_NewCFunction(context, NativeCreateSceneLayer, "createSceneLayer", 1));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "sortSceneLayer",
                      JS_NewCFunction(context, NativeSortSceneLayer, "sortSceneLayer", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "getSceneLayerIndex",
                      JS_NewCFunction(context, NativeGetSceneLayerIndex, "getSceneLayerIndex", 1));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "getInitialSceneLayerConfig",
        JS_NewCFunction(
            context, NativeGetInitialSceneLayerConfig, "getInitialSceneLayerConfig", 1));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "resolveLayerAnimation",
        JS_NewCFunction(context, NativeResolveLayerAnimation, "resolveLayerAnimation", 2));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "resolvePropertyAnimation",
        JS_NewCFunction(context, NativeResolvePropertyAnimation, "resolvePropertyAnimation", 2));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "getPropertyAnimationProperty",
        JS_NewCFunction(
            context, NativeGetPropertyAnimationProperty, "getPropertyAnimationProperty", 2));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "setPropertyAnimationProperty",
        JS_NewCFunction(
            context, NativeSetPropertyAnimationProperty, "setPropertyAnimationProperty", 3));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "propertyAnimationCall",
        JS_NewCFunction(context, NativePropertyAnimationCall, "propertyAnimationCall", 3));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "setTimer",
                      JS_NewCFunction(context, NativeSetTimer, "setTimer", 4));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "clearTimer",
                      JS_NewCFunction(context, NativeClearTimer, "clearTimer", 1));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "localStorageGet",
                      JS_NewCFunction(context, NativeLocalStorageGet, "localStorageGet", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "localStorageSet",
                      JS_NewCFunction(context, NativeLocalStorageSet, "localStorageSet", 3));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "localStorageClear",
                      JS_NewCFunction(context, NativeLocalStorageClear, "localStorageClear", 1));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "localStorageDelete",
                      JS_NewCFunction(context, NativeLocalStorageDelete, "localStorageDelete", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "layerCall",
                      JS_NewCFunction(context, NativeLayerCall, "layerCall", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "hasVideoTexture",
                      JS_NewCFunction(context, NativeHasVideoTexture, "hasVideoTexture", 1));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "videoTextureCall",
                      JS_NewCFunction(context, NativeVideoTextureCall, "videoTextureCall", 2));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "hasTextureAnimation",
        JS_NewCFunction(context, NativeHasTextureAnimation, "hasTextureAnimation", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "isDeferredRuntimeLayer",
                      JS_NewCFunction(
                          context, NativeIsDeferredRuntimeLayer, "isDeferredRuntimeLayer", 1));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "getAnimationLayerCount",
        JS_NewCFunction(context, NativeGetAnimationLayerCount, "getAnimationLayerCount", 1));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "resolveAnimationLayer",
        JS_NewCFunction(context, NativeResolveAnimationLayer, "resolveAnimationLayer", 2));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "hasAnimationLayer",
                      JS_NewCFunction(context, NativeHasAnimationLayer, "hasAnimationLayer", 2));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "hasAnimationLayerMember",
        JS_NewCFunction(context, NativeHasAnimationLayerMember, "hasAnimationLayerMember", 3));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "getAnimationLayerProperty",
        JS_NewCFunction(context, NativeGetAnimationLayerProperty, "getAnimationLayerProperty", 3));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "setAnimationLayerProperty",
        JS_NewCFunction(context, NativeSetAnimationLayerProperty, "setAnimationLayerProperty", 4));
    JS_SetPropertyStr(context,
                      m_impl->native_bridge,
                      "animationLayerCall",
                      JS_NewCFunction(context, NativeAnimationLayerCall, "animationLayerCall", 4));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "addAnimationLayerEndedCallback",
        JS_NewCFunction(
            context, NativeAddAnimationLayerEndedCallback, "addAnimationLayerEndedCallback", 3));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "textureAnimationGet",
        JS_NewCFunction(context, NativeTextureAnimationGet, "textureAnimationGet", 3));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "textureAnimationSet",
        JS_NewCFunction(context, NativeTextureAnimationSet, "textureAnimationSet", 4));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "textureAnimationCall",
        JS_NewCFunction(context, NativeTextureAnimationCall, "textureAnimationCall", 4));
    JS_SetPropertyStr(
        context,
        m_impl->native_bridge,
        "rotateLayerObjectSpace",
        JS_NewCFunction(context, NativeRotateLayerObjectSpace, "rotateLayerObjectSpace", 2));

    JS_SetPropertyStr(context,
                      m_impl->engine_base,
                      "userProperties",
                      JS_DupValue(context, m_impl->user_properties_object));
    JS_SetPropertyStr(context, m_impl->engine_base, "canvasSize", JS_NewObject(context));
    JS_SetPropertyStr(context, m_impl->engine_base, "screenResolution", JS_NewObject(context));
    JS_SetPropertyStr(
        context, m_impl->engine_base, "frametime", JS_NewFloat64(context, 1.0 / 60.0));
    JS_SetPropertyStr(context, m_impl->engine_base, "runtime", JS_NewFloat64(context, 0.0));
    JS_SetPropertyStr(
        context, m_impl->engine_base, "timeOfDay", JS_NewFloat64(context, ComputeTimeOfDay()));
    JS_SetPropertyStr(
        context, m_impl->engine_base, "AUDIO_RESOLUTION_16", JS_NewInt32(context, 16));
    JS_SetPropertyStr(
        context, m_impl->engine_base, "AUDIO_RESOLUTION_32", JS_NewInt32(context, 32));
    JS_SetPropertyStr(
        context, m_impl->engine_base, "AUDIO_RESOLUTION_64", JS_NewInt32(context, 64));
    JS_SetPropertyStr(
        context,
        m_impl->engine_base,
        "registerAudioBuffers",
        JS_NewCFunction(context, NativeRegisterAudioBuffers, "registerAudioBuffers", 1));
    JS_SetPropertyStr(context, m_impl->input, "cursorWorldPosition", JS_NewObject(context));
    JS_SetPropertyStr(context, m_impl->input, "cursorScreenPosition", JS_NewObject(context));
    JS_SetPropertyStr(context, m_impl->input, "cursorLeftDown", JS_FALSE);

    auto set_vec2 = [context](JSValueConst object, double x, double y) {
        JS_SetPropertyStr(context, object, "x", JS_NewFloat64(context, x));
        JS_SetPropertyStr(context, object, "y", JS_NewFloat64(context, y));
    };

    JSValue canvas_size = JS_GetPropertyStr(context, m_impl->engine_base, "canvasSize");
    JSValue screen_size = JS_GetPropertyStr(context, m_impl->engine_base, "screenResolution");
    if (m_scene != nullptr) {
        set_vec2(canvas_size, m_scene->ortho[0], m_scene->ortho[1]);
        set_vec2(screen_size, m_scene->ortho[0], m_scene->ortho[1]);
    }
    JS_FreeValue(context, canvas_size);
    JS_FreeValue(context, screen_size);

    UpdateInputState(m_impl);
    UpdateGeneralSettingsObject(context,
                                m_impl->general_settings_object,
                                m_impl->general_settings,
                                m_impl->general_setting_names);
    UpdateUserPropertyObject(context,
                             m_impl->user_properties_object,
                             m_impl->user_properties,
                             m_impl->user_property_names);
    LoadLocalStorage(m_impl);
}

WPSceneScriptHost::~WPSceneScriptHost() {
    if (m_impl == nullptr) return;

    if (m_impl->runtime.context != nullptr) {
        for (auto& instance : m_impl->instances) {
            if (instance->initialized && ! JS_IsUndefined(instance->destroy_fn)) {
                JSValue result = JS_Call(
                    m_impl->runtime.context, instance->destroy_fn, JS_UNDEFINED, 0, nullptr);
                if (JS_IsException(result)) {
                    LogQuickJSException(m_impl->runtime.context, "destroy");
                }
                JS_FreeValue(m_impl->runtime.context, result);
            }

            FreeJSValue(m_impl->runtime.context, instance->script_properties);
            FreeJSValue(m_impl->runtime.context, instance->exports);
            FreeJSValue(m_impl->runtime.context, instance->init_fn);
            FreeJSValue(m_impl->runtime.context, instance->update_fn);
            FreeJSValue(m_impl->runtime.context, instance->apply_user_properties_fn);
            FreeJSValue(m_impl->runtime.context, instance->apply_general_settings_fn);
            FreeJSValue(m_impl->runtime.context, instance->cursor_enter_fn);
            FreeJSValue(m_impl->runtime.context, instance->cursor_leave_fn);
            FreeJSValue(m_impl->runtime.context, instance->cursor_move_fn);
            FreeJSValue(m_impl->runtime.context, instance->cursor_down_fn);
            FreeJSValue(m_impl->runtime.context, instance->cursor_up_fn);
            FreeJSValue(m_impl->runtime.context, instance->cursor_click_fn);
            FreeJSValue(m_impl->runtime.context, instance->media_thumbnail_changed_fn);
            FreeJSValue(m_impl->runtime.context, instance->media_properties_changed_fn);
            FreeJSValue(m_impl->runtime.context, instance->media_playback_changed_fn);
            FreeJSValue(m_impl->runtime.context, instance->destroy_fn);
            FreeJSValue(m_impl->runtime.context, instance->resize_screen_fn);
        }

        for (auto& timer : m_impl->timers) {
            FreeJSValue(m_impl->runtime.context, timer.callback);
        }

        for (auto& binding : m_impl->audio_buffers) {
            FreeJSValue(m_impl->runtime.context, binding.object);
            FreeJSValue(m_impl->runtime.context, binding.left);
            FreeJSValue(m_impl->runtime.context, binding.right);
            FreeJSValue(m_impl->runtime.context, binding.average);
        }

        for (auto& [node, layers] : m_impl->animation_layer_states) {
            (void)node;
            for (auto& [index, state] : layers) {
                (void)index;
                for (auto& callback : state.ended_callbacks) {
                    FreeJSValue(m_impl->runtime.context, callback);
                }
            }
        }

        FreeJSValue(m_impl->runtime.context, m_impl->shared);
        FreeJSValue(m_impl->runtime.context, m_impl->engine_base);
        FreeJSValue(m_impl->runtime.context, m_impl->console);
        FreeJSValue(m_impl->runtime.context, m_impl->input);
        FreeJSValue(m_impl->runtime.context, m_impl->general_settings_object);
        FreeJSValue(m_impl->runtime.context, m_impl->scene_object);
        FreeJSValue(m_impl->runtime.context, m_impl->native_bridge);
        FreeJSValue(m_impl->runtime.context, m_impl->user_properties_object);
        JS_FreeContext(m_impl->runtime.context);
    }

    if (m_impl->runtime.runtime != nullptr) {
        JS_FreeRuntime(m_impl->runtime.runtime);
    }

    delete m_impl;
}

bool WPSceneScriptHost::Ready() const noexcept {
    return m_impl != nullptr && m_impl->runtime.runtime != nullptr &&
           m_impl->runtime.context != nullptr;
}

bool WPSceneScriptHost::RegisterPropertyBinding(WPSceneScriptRegistration registration) {
    const bool scene_registration = registration.target_kind == WPSceneScriptTargetKind::Scene;
    const bool sound_registration = registration.target_kind == WPSceneScriptTargetKind::Sound;
    // Scene-level bindings do not have a SceneNode owner. Keep the node requirement for layer-like
    // targets, but allow `general` properties and mounted sound streams to enter the same
    // user-property dispatch table through their target-specific apply functions.
    if (! Ready() ||
        (! scene_registration && ! sound_registration && registration.node == nullptr) ||
        ! registration.setting.hasUserBinding()) {
        return false;
    }

    if (m_impl->initialized) {
        WPScriptEvaluationContext base_context;
        base_context.canvas_size = {
            m_scene != nullptr ? static_cast<double>(m_scene->ortho[0]) : 0.0,
            m_scene != nullptr ? static_cast<double>(m_scene->ortho[1]) : 0.0,
        };
        const auto value =
            EvaluateRegistrationSetting(registration, &m_impl->user_properties, base_context);
        ApplyRegistrationValue(m_impl, registration, value);
    }

    m_impl->property_bindings.push_back(std::move(registration));
    return true;
}

bool WPSceneScriptHost::RegisterPropertyScript(WPSceneScriptRegistration registration) {
    if (! Ready() || registration.node == nullptr || ! registration.setting.hasScript())
        return false;

    auto instance           = std::make_unique<ScriptInstance>();
    instance->instance_id   = m_impl->next_instance_id++;
    instance->registration  = std::move(registration);
    instance->current_value = instance->registration.setting.value;
    m_impl->instances.push_back(std::move(instance));

    auto* node = m_impl->instances.back()->registration.node;
    EnsureTextureAnimationStatesForNode(m_impl, node);

    if (m_impl->initialized) {
        InitializeScriptInstance(m_impl, *m_impl->instances.back());
        RunScriptInstanceInit(m_impl, *m_impl->instances.back());
    }

    return true;
}

bool WPSceneScriptHost::RegisterPropertyAnimation(WPSceneScriptRegistration registration) {
    if (! Ready() || registration.node == nullptr || registration.animation == nullptr ||
        ! registration.animation->valid()) {
        return false;
    }

    if (m_impl->initialized && registration.setting.hasUserBinding()) {
        WPScriptEvaluationContext base_context;
        base_context.canvas_size = {
            m_scene != nullptr ? static_cast<double>(m_scene->ortho[0]) : 0.0,
            m_scene != nullptr ? static_cast<double>(m_scene->ortho[1]) : 0.0,
        };
        registration.base_value =
            EvaluateRegistrationSetting(registration, &m_impl->user_properties, base_context);
    }

    PropertyAnimationInstance animation;
    animation.registration       = std::move(registration);
    animation.animation_id       = m_impl->next_property_animation_id++;
    animation.state.animation_id = animation.animation_id;
    InitializePropertyAnimationState(*animation.registration.animation, animation.state);
    m_impl->property_animations.push_back(std::move(animation));
    ApplyPropertyAnimationInstance(m_impl, m_impl->property_animations.back());
    return true;
}

void WPSceneScriptHost::Initialize() {
    if (! Ready()) return;

    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        InitializeScriptInstance(m_impl, instance);
    }

    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        RunScriptInstanceInit(m_impl, instance);
    }

    m_impl->initialized = true;
    ApplyGeneralSettings(m_impl->general_settings, true);
    ApplyUserProperties(m_impl->user_properties, true);
    ApplyMediaState(m_impl->media_state, true);

    m_impl->deferred_residency_warmup_layer_ids.clear();
    m_impl->deferred_residency_warmup_index = 0;
    std::unordered_set<int32_t> queued;
    auto                        append_if_deferred = [&](int32_t layer_id) {
        if (layer_id == 0 || ! queued.insert(layer_id).second) return;
        if (! IsDeferredRuntimeLayer(m_impl, layer_id)) return;
        m_impl->deferred_residency_warmup_layer_ids.push_back(layer_id);
    };
    for (const auto layer_id : m_scene->layerOrder) append_if_deferred(layer_id);
    for (const auto layer_id : m_scene->deferredRuntimeImageLayerIds) append_if_deferred(layer_id);
    for (const auto layer_id : m_scene->deferredRuntimeParticleLayerIds)
        append_if_deferred(layer_id);
    for (const auto layer_id : m_scene->deferredRuntimeTextLayerIds) append_if_deferred(layer_id);
    LOG_VERBOSE("DeferredRuntimeResidencyWarmupQueued: count=%zu",
                m_impl->deferred_residency_warmup_layer_ids.size());
}

bool WPSceneScriptHost::MaterializeNextDeferredRuntimeLayerForResidency() {
    if (! Ready() || m_scene == nullptr) return false;

    while (m_impl->deferred_residency_warmup_index <
           m_impl->deferred_residency_warmup_layer_ids.size()) {
        const auto layer_id =
            m_impl->deferred_residency_warmup_layer_ids[m_impl->deferred_residency_warmup_index++];
        if (! IsDeferredRuntimeLayer(m_impl, layer_id)) continue;

        const auto started_at = std::chrono::steady_clock::now();
        // Initial scripts already materialize any branch that must be visible for the first frame.
        // Remaining queued layers are hidden, so inserting their CPU/runtime identity does not
        // change the active graph. Keep the fallback for an unexpectedly visible layer correct.
        const bool mark_graph_dirty = m_scene->IsLayerVisible(layer_id);
        bool       materialized     = true;
        materialized = MaterializeDeferredImageLayerIfNeeded(m_impl, layer_id, mark_graph_dirty) &&
                       materialized;
        materialized =
            MaterializeDeferredParticleLayerIfNeeded(m_impl, layer_id, mark_graph_dirty) &&
            materialized;
        materialized = MaterializeDeferredTextLayerIfNeeded(m_impl, layer_id, mark_graph_dirty) &&
                       materialized;

        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - started_at)
                                    .count();
        LOG_VERBOSE("DeferredRuntimeResidencyWarmupStep: layer=%d materialized=%s "
                    "topology-dirty=%s remaining=%zu duration=%.2fms",
                    layer_id,
                    materialized ? "true" : "false",
                    mark_graph_dirty ? "true" : "false",
                    m_impl->deferred_residency_warmup_layer_ids.size() -
                        m_impl->deferred_residency_warmup_index,
                    static_cast<double>(elapsed_us) / 1000.0);
        return true;
    }

    return false;
}

void WPSceneScriptHost::FrameBegin(double frame_time) {
    if (! Ready()) return;

    ProcessPendingSceneLayerDestroy(m_impl);
    m_impl->runtime_seconds += std::max(0.0, frame_time);

    JSContext* context = m_impl->runtime.context;
    JS_SetPropertyStr(
        context, m_impl->engine_base, "frametime", JS_NewFloat64(context, frame_time));
    JS_SetPropertyStr(
        context, m_impl->engine_base, "runtime", JS_NewFloat64(context, m_impl->runtime_seconds));
    JS_SetPropertyStr(
        context, m_impl->engine_base, "timeOfDay", JS_NewFloat64(context, ComputeTimeOfDay()));
    UpdateInputState(m_impl);
    UpdateAudioBufferBindings(m_impl);
    UpdatePropertyAnimations(m_impl, frame_time);

    const double             frame_ms = std::max(0.0, frame_time * 1000.0);
    std::vector<ScriptTimer> due_timers;
    due_timers.reserve(m_impl->timers.size());

    for (auto it = m_impl->timers.begin(); it != m_impl->timers.end();) {
        it->remaining_ms -= frame_ms;
        if (it->remaining_ms > 0.0) {
            ++it;
            continue;
        }

        ScriptTimer timer = *it;
        timer.callback    = JS_DupValue(context, it->callback);

        if (it->repeat) {
            it->remaining_ms += std::max(0.0, it->interval_ms);
            ++it;
        } else {
            FreeJSValue(context, it->callback);
            it = m_impl->timers.erase(it);
        }

        due_timers.push_back(std::move(timer));
    }

    for (auto& timer : due_timers) {
        JSValue result = JS_Call(context, timer.callback, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(result)) {
            LOG_ERROR("QuickJS timer context: %s, timerId=%llu, repeat=%s, intervalMs=%.3f",
                      DescribeScriptInstance(m_impl, timer.owner_instance_id).c_str(),
                      static_cast<unsigned long long>(timer.id),
                      timer.repeat ? "true" : "false",
                      timer.interval_ms);
            LogQuickJSException(context, "timer");
        }
        JS_FreeValue(context, result);
        JS_FreeValue(context, timer.callback);
        timer.callback = JS_UNDEFINED;
    }

    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        if (! instance.initialized || JS_IsUndefined(instance.update_fn)) continue;

        if (const auto node_value = ReadRegistrationValue(m_impl, instance.registration);
            node_value.has_value()) {
            instance.current_value = *node_value;
        }

        JSValue result = JS_UNDEFINED;
        const auto script_facing_value =
            ToScriptFacingRegistrationValue(instance.registration, instance.current_value);
        if (const auto script_value = script_facing_value.toScriptValue(); script_value.has_value()) {
            JSValue arg = ScriptValueToJS(context, *script_value);
            result      = JS_Call(context, instance.update_fn, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(context, arg);
        } else {
            result = JS_Call(context, instance.update_fn, JS_UNDEFINED, 0, nullptr);
        }

        if (JS_IsException(result)) {
            LogQuickJSException(context, "update");
            JS_FreeValue(context, result);
            continue;
        }

        if (! JS_IsUndefined(result)) {
            const auto hint  = RegistrationValueType(instance.registration);
            const auto value = ReadDynamicValueFromJS(
                context, result, hint.supported ? hint.type : instance.current_value.type());
            if (value.has_value()) {
                const auto runtime_value =
                    FromScriptFacingRegistrationValue(instance.registration, *value);
                // The Wallpaper Engine script convention is often "return the current value" from
                // every update tick. Avoid re-entering the registration writer for identical
                // values; the writer still performs its own guard for external callers, but
                // skipping here removes a full property read/compare/apply round from every
                // steady-state script.
                if (instance.current_value.equals(runtime_value)) {
                    JS_FreeValue(context, result);
                    continue;
                }
                instance.current_value = runtime_value;
                ApplyRegistrationValue(m_impl, instance.registration, runtime_value);
            }
        }

        JS_FreeValue(context, result);
    }
}

void WPSceneScriptHost::ApplyAudioSamples(const std::vector<float>& audio_samples) {
    m_impl->external_audio_samples = audio_samples;
    if (Ready()) UpdateAudioBufferBindings(m_impl);
}

bool WPSceneScriptHost::GetAudioSpectrum(uint32_t resolution, std::vector<float>* left,
                                         std::vector<float>* right,
                                         std::vector<float>* average) const {
    if (left == nullptr || right == nullptr || average == nullptr || m_impl == nullptr)
        return false;

    if (GetExternalAudioBufferValues(m_impl, resolution, left, right, average)) return true;
    if (m_scene == nullptr || m_scene->soundManager == nullptr) return false;

    m_scene->soundManager->GetSpectrum(resolution, left, right, average);
    return ! left->empty() || ! right->empty() || ! average->empty();
}

void WPSceneScriptHost::ApplyUserProperties(const UserPropertyMap& user_properties,
                                            bool                   initial_dispatch) {
    if (! Ready()) return;

    const bool was_applying_user_properties = m_impl->applying_user_properties;
    m_impl->applying_user_properties = true;
    m_impl->user_properties = user_properties;

    JSContext* context = m_impl->runtime.context;
    UpdateUserPropertyObject(context,
                             m_impl->user_properties_object,
                             m_impl->user_properties,
                             m_impl->user_property_names);

    WPScriptEvaluationContext base_context;
    base_context.canvas_size = {
        m_scene != nullptr ? static_cast<double>(m_scene->ortho[0]) : 0.0,
        m_scene != nullptr ? static_cast<double>(m_scene->ortho[1]) : 0.0,
    };

    const UserPropertyMap* previous_properties =
        initial_dispatch ? nullptr : &m_impl->dispatched_user_properties;
    JSValue changed = MakeChangedUserProperties(context, user_properties, previous_properties);

    for (auto& registration : m_impl->property_bindings) {
        if (! registration.setting.hasUserBinding()) continue;
        const auto value =
            EvaluateRegistrationSetting(registration, &m_impl->user_properties, base_context);
        ApplyRegistrationValue(m_impl, registration, value);
    }

    for (auto& animation : m_impl->property_animations) {
        if (! animation.registration.setting.hasUserBinding()) continue;
        animation.registration.base_value = EvaluateRegistrationSetting(
            animation.registration, &m_impl->user_properties, base_context);
        ApplyPropertyAnimationInstance(m_impl, animation);
    }

    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        if (instance.registration.setting.hasUserBinding()) {
            // Script init() intentionally saw the authored value during bootstrap. Apply every
            // user-bound script property here, not only visibility, so the post-init user override
            // phase remains complete for origin, scale, uniforms, and any future script target.
            const auto resolved = EvaluateRegistrationSetting(
                instance.registration, &m_impl->user_properties, base_context);
            instance.current_value = resolved;
            ApplyRegistrationValue(m_impl, instance.registration, resolved);
        }

        for (const auto& [name, setting] : instance.registration.setting.script_properties) {
            (void)name;
            const auto resolved =
                setting->evaluate(&m_impl->user_properties, nullptr, base_context);
            const auto script_value = resolved.toScriptValue();
            if (! script_value.has_value()) continue;
            SetJSProperty(context, instance.script_properties, name.c_str(), *script_value);
        }
    }

    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        if (! instance.initialized || JS_IsUndefined(instance.apply_user_properties_fn)) continue;

        JSValue arg    = JS_DupValue(context, changed);
        JSValue result = JS_Call(context, instance.apply_user_properties_fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(context, arg);
        if (JS_IsException(result)) {
            LogQuickJSException(context, "applyUserProperties");
        }
        JS_FreeValue(context, result);
    }
    m_impl->dispatched_user_properties = user_properties;
    JS_FreeValue(context, changed);

    if (initial_dispatch && m_scene != nullptr) {
        LogSceneTextLayerState(*m_scene, 258, "Text258");
        LogSceneTextLayerState(*m_scene, 282, "Text282");
        LogSceneLayer332State(*m_scene);
    }

    m_impl->applying_user_properties = was_applying_user_properties;
    if (! m_impl->applying_user_properties) {
        FlushPendingSceneRegistrationRanges(m_impl);
    }
}

void WPSceneScriptHost::ApplyGeneralSettings(
    const std::unordered_map<std::string, std::string>& general_settings, bool initial_dispatch) {
    if (! Ready()) return;

    m_impl->general_settings = general_settings;

    JSContext* context = m_impl->runtime.context;
    UpdateGeneralSettingsObject(context,
                                m_impl->general_settings_object,
                                m_impl->general_settings,
                                m_impl->general_setting_names);

    const auto* previous_settings =
        initial_dispatch ? nullptr : &m_impl->dispatched_general_settings;
    JSValue changed =
        MakeChangedGeneralSettings(context, m_impl->general_settings, previous_settings);

    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        if (! instance.initialized || JS_IsUndefined(instance.apply_general_settings_fn)) continue;

        JSValue arg = JS_DupValue(context, changed);
        JSValue result =
            JS_Call(context, instance.apply_general_settings_fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(context, arg);
        if (JS_IsException(result)) {
            LogQuickJSException(context, "applyGeneralSettings");
        }
        JS_FreeValue(context, result);
    }

    m_impl->dispatched_general_settings = m_impl->general_settings;
    JS_FreeValue(context, changed);
}

void WPSceneScriptHost::ApplyMediaState(const WPSceneScriptMediaState& media_state,
                                        bool                           initial_dispatch) {
    if (! Ready()) return;

    m_impl->media_state = media_state;
    UpdateMediaThumbnailTexture(m_impl, media_state);

    JSContext* context = m_impl->runtime.context;
    const bool thumbnail_changed =
        initial_dispatch ||
        m_impl->dispatched_media_state.has_thumbnail != media_state.has_thumbnail ||
        m_impl->dispatched_media_state.primary_color != media_state.primary_color ||
        m_impl->dispatched_media_state.secondary_color != media_state.secondary_color ||
        m_impl->dispatched_media_state.tertiary_color != media_state.tertiary_color ||
        m_impl->dispatched_media_state.text_color != media_state.text_color ||
        m_impl->dispatched_media_state.high_contrast_color != media_state.high_contrast_color ||
        m_impl->dispatched_media_state.thumbnail_width != media_state.thumbnail_width ||
        m_impl->dispatched_media_state.thumbnail_height != media_state.thumbnail_height ||
        m_impl->dispatched_media_state.thumbnail_rgba != media_state.thumbnail_rgba ||
        m_impl->dispatched_media_state.previous_thumbnail_width !=
            media_state.previous_thumbnail_width ||
        m_impl->dispatched_media_state.previous_thumbnail_height !=
            media_state.previous_thumbnail_height ||
        m_impl->dispatched_media_state.previous_thumbnail_rgba !=
            media_state.previous_thumbnail_rgba;
    const bool properties_changed = initial_dispatch ||
                                    m_impl->dispatched_media_state.title != media_state.title ||
                                    m_impl->dispatched_media_state.artist != media_state.artist ||
                                    m_impl->dispatched_media_state.album_title !=
                                        media_state.album_title ||
                                    m_impl->dispatched_media_state.album_artist !=
                                        media_state.album_artist ||
                                    m_impl->dispatched_media_state.sub_title !=
                                        media_state.sub_title ||
                                    m_impl->dispatched_media_state.genres != media_state.genres ||
                                    m_impl->dispatched_media_state.content_type !=
                                        media_state.content_type;
    const bool playback_changed =
        initial_dispatch ||
        m_impl->dispatched_media_state.playback_state != media_state.playback_state;

    JSValue thumbnail_event  = JS_UNDEFINED;
    JSValue properties_event = JS_UNDEFINED;
    JSValue playback_event   = JS_UNDEFINED;
    if (thumbnail_changed) thumbnail_event = MakeMediaThumbnailEvent(context, media_state);
    if (properties_changed) properties_event = MakeMediaPropertiesEvent(context, media_state);
    if (playback_changed) playback_event = MakeMediaPlaybackEvent(context, media_state);

    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        if (! instance.initialized) continue;

        if (thumbnail_changed && ! JS_IsUndefined(instance.media_thumbnail_changed_fn)) {
            JSValue arg = JS_DupValue(context, thumbnail_event);
            CallScriptEvent(
                context, instance.media_thumbnail_changed_fn, arg, "mediaThumbnailChanged");
            JS_FreeValue(context, arg);
        }
        if (properties_changed && ! JS_IsUndefined(instance.media_properties_changed_fn)) {
            JSValue arg = JS_DupValue(context, properties_event);
            CallScriptEvent(
                context, instance.media_properties_changed_fn, arg, "mediaPropertiesChanged");
            JS_FreeValue(context, arg);
        }
        if (playback_changed && ! JS_IsUndefined(instance.media_playback_changed_fn)) {
            JSValue arg = JS_DupValue(context, playback_event);
            CallScriptEvent(
                context, instance.media_playback_changed_fn, arg, "mediaPlaybackChanged");
            JS_FreeValue(context, arg);
        }
    }

    if (! JS_IsUndefined(thumbnail_event)) JS_FreeValue(context, thumbnail_event);
    if (! JS_IsUndefined(properties_event)) JS_FreeValue(context, properties_event);
    if (! JS_IsUndefined(playback_event)) JS_FreeValue(context, playback_event);
    m_impl->dispatched_media_state = media_state;
}

void WPSceneScriptHost::HandleCursorMove() {
    if (! Ready() || m_impl->scene == nullptr) return;

    UpdateInputState(m_impl);
    JSContext* context   = m_impl->runtime.context;
    const auto cursor    = ComputeCursorPositionState(m_impl);
    const bool left_down = m_impl->scene->cursorLeftDown;

    std::unordered_set<uint32_t> next_hovered;
    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        if (! instance.initialized) continue;
        if (! InstanceReceivesCursor(m_impl, instance, cursor)) continue;
        next_hovered.insert(instance.instance_id);
    }

    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        if (! instance.initialized) continue;

        const bool was_hovered = m_impl->hovered_instances.contains(instance.instance_id);
        const bool is_hovered  = next_hovered.contains(instance.instance_id);
        if (was_hovered == is_hovered) continue;

        JSValue event = MakeCursorEventObject(context, cursor, left_down);
        if (is_hovered && ! JS_IsUndefined(instance.cursor_enter_fn)) {
            CallScriptEvent(context, instance.cursor_enter_fn, event, "cursorEnter");
        } else if (! is_hovered && ! JS_IsUndefined(instance.cursor_leave_fn)) {
            CallScriptEvent(context, instance.cursor_leave_fn, event, "cursorLeave");
        }
        JS_FreeValue(context, event);
    }

    m_impl->hovered_instances = std::move(next_hovered);

    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        if (! instance.initialized || ! m_impl->hovered_instances.contains(instance.instance_id) ||
            JS_IsUndefined(instance.cursor_move_fn)) {
            continue;
        }

        JSValue event = MakeCursorEventObject(context, cursor, left_down);
        CallScriptEvent(context, instance.cursor_move_fn, event, "cursorMove");
        JS_FreeValue(context, event);
    }
}

void WPSceneScriptHost::HandleCursorButton(bool down) {
    if (! Ready() || m_impl->scene == nullptr) return;

    m_impl->scene->cursorLeftDown = down;
    UpdateInputState(m_impl);

    JSContext* context = m_impl->runtime.context;
    const auto cursor  = ComputeCursorPositionState(m_impl);

    if (down) {
        HandleCursorMove();
        m_impl->pressed_instances = m_impl->hovered_instances;
        for (const auto& instance_ptr : m_impl->instances) {
            auto& instance = *instance_ptr;
            if (! instance.initialized ||
                ! m_impl->pressed_instances.contains(instance.instance_id) ||
                JS_IsUndefined(instance.cursor_down_fn)) {
                continue;
            }

            JSValue event = MakeCursorEventObject(context, cursor, true);
            CallScriptEvent(context, instance.cursor_down_fn, event, "cursorDown");
            JS_FreeValue(context, event);
        }
        return;
    }

    HandleCursorMove();
    const auto pressed = m_impl->pressed_instances;
    m_impl->pressed_instances.clear();
    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        if (! instance.initialized || ! pressed.contains(instance.instance_id)) continue;

        JSValue event = MakeCursorEventObject(context, cursor, false);
        if (! JS_IsUndefined(instance.cursor_up_fn)) {
            CallScriptEvent(context, instance.cursor_up_fn, event, "cursorUp");
        }
        if (m_impl->hovered_instances.contains(instance.instance_id) &&
            ! JS_IsUndefined(instance.cursor_click_fn)) {
            CallScriptEvent(context, instance.cursor_click_fn, event, "cursorClick");
        }
        JS_FreeValue(context, event);
    }
}

void WPSceneScriptHost::ResizeScreen(int32_t width, int32_t height) {
    if (! Ready()) return;

    JSContext* context = m_impl->runtime.context;
    JSValue    screen  = JS_GetPropertyStr(context, m_impl->engine_base, "screenResolution");
    JS_SetPropertyStr(context, screen, "x", JS_NewFloat64(context, width));
    JS_SetPropertyStr(context, screen, "y", JS_NewFloat64(context, height));
    JS_FreeValue(context, screen);

    for (const auto& instance_ptr : m_impl->instances) {
        auto& instance = *instance_ptr;
        if (! instance.initialized || JS_IsUndefined(instance.resize_screen_fn)) continue;

        JSValue arg =
            NumericVectorToJS(context, { static_cast<double>(width), static_cast<double>(height) });
        JSValue result = JS_Call(context, instance.resize_screen_fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(context, arg);
        if (JS_IsException(result)) {
            LogQuickJSException(context, "resizeScreen");
        }
        JS_FreeValue(context, result);
    }
}

void WPSceneScriptHost::ApplyTextureAnimations(SceneNode* node, sprite_map_t& sprites,
                                               double frame_time) {
    if (! Ready() || node == nullptr) return;

    auto states_it = m_impl->texture_states.find(node);
    if (states_it == m_impl->texture_states.end()) return;

    for (auto& [slot, state] : states_it->second) {
        if (! exists(sprites, slot)) continue;
        state.animation.GetAnimateFrame(frame_time * state.rate);
        sprites[slot] = state.animation;
    }
}

void WPSceneScriptHost::NotifyAnimationLayersAdvanced(SceneNode* node) {
    if (! Ready() || node == nullptr) return;

    auto* data = GetNodeData(m_impl, node);
    if (data == nullptr || ! data->puppet_layer.hasPuppet()) return;

    JSContext* context = m_impl->runtime.context;
    auto&      states  = m_impl->animation_layer_states[node];

    for (usize index = 0; index < data->puppet_layer.AnimationLayerCount(); index++) {
        auto* layer = data->puppet_layer.AnimationLayerState(index);
        if (layer == nullptr || data->puppet_layer.AnimationDefinition(index) == nullptr) continue;

        auto& state = states[index];
        if (! state.seen) {
            state.last_time = layer->cur_time;
            state.seen      = true;
            continue;
        }

        const bool wrapped = layer->playing && layer->cur_time < state.last_time;
        // Single-shot layers do not wrap, so they arm an explicit completion latch when the
        // update step stops them on their final authored frame.
        const bool completed_single = layer->pending_ended_callback;
        state.last_time             = layer->cur_time;
        if (! wrapped && ! completed_single) continue;

        for (const auto& callback : state.ended_callbacks) {
            JSValue result = JS_Call(context, callback, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(result)) {
                LogQuickJSException(context, "animationLayerEnded");
            }
            JS_FreeValue(context, result);
        }
        layer->pending_ended_callback = false;
    }
}

} // namespace wallpaper
