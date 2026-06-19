#include "scenescript/WPSceneScriptHost.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "scene/Scene.h"
#include "scene/SceneMaterial.h"
#include "scene/SceneMesh.h"
#include "scene/SceneNode.h"

#include "parser/WPSyntheticImageParser.hpp"
#include "scenescript/WPScriptRuntime.hpp"

namespace wallpaper
{
namespace
{

struct AnimationInstance {
    WPSceneScriptRegistration registration;
    WPPropertyAnimationState state;
};

std::string MakeAnimationKey(int32_t object_id, std::string_view property_name) {
    return std::to_string(object_id) + ":" + std::string(property_name);
}

std::string MakeRegistrationKey(const WPSceneScriptRegistration& registration) {
    return MakeAnimationKey(registration.object_id, registration.property_name);
}

void InitializeAnimationInstance(AnimationInstance& instance) {
    if (!instance.registration.animation) {
        return;
    }

    InitializePropertyAnimationState(*instance.registration.animation, instance.state);
}

WPScriptValue ToScriptValue(const WPDynamicValue& value) {
    switch (value.type()) {
    case WPDynamicValue::Type::Boolean: {
        bool out = false;
        value.tryGet(&out);
        return WPScriptValue::Boolean(out);
    }
    case WPDynamicValue::Type::String: {
        std::string out;
        value.tryGet(&out);
        return WPScriptValue::String(std::move(out));
    }
    case WPDynamicValue::Type::Int32: {
        int32_t out = 0;
        value.tryGet(&out);
        return WPScriptValue::Number(static_cast<double>(out));
    }
    case WPDynamicValue::Type::UInt32: {
        uint32_t out = 0;
        value.tryGet(&out);
        return WPScriptValue::Number(static_cast<double>(out));
    }
    case WPDynamicValue::Type::Float: {
        float out = 0.0f;
        value.tryGet(&out);
        return WPScriptValue::Number(static_cast<double>(out));
    }
    case WPDynamicValue::Type::Double: {
        double out = 0.0;
        value.tryGet(&out);
        return WPScriptValue::Number(out);
    }
    case WPDynamicValue::Type::Float2: {
        std::array<float, 2> out {};
        value.tryGet(&out);
        return WPScriptValue::NumberArray({ out[0], out[1] });
    }
    case WPDynamicValue::Type::Float3: {
        std::array<float, 3> out {};
        value.tryGet(&out);
        return WPScriptValue::NumberArray({ out[0], out[1], out[2] });
    }
    case WPDynamicValue::Type::Float4: {
        std::array<float, 4> out {};
        value.tryGet(&out);
        return WPScriptValue::NumberArray({ out[0], out[1], out[2], out[3] });
    }
    case WPDynamicValue::Type::FloatVector: {
        std::vector<float> out;
        value.tryGet(&out);
        std::vector<double> values;
        values.reserve(out.size());
        for (const float component : out) {
            values.push_back(component);
        }
        return WPScriptValue::NumberArray(std::move(values));
    }
    case WPDynamicValue::Type::Int3: {
        std::array<int32_t, 3> out {};
        value.tryGet(&out);
        return WPScriptValue::NumberArray(
            { static_cast<double>(out[0]), static_cast<double>(out[1]), static_cast<double>(out[2]) });
    }
    case WPDynamicValue::Type::Null:
        break;
    }

    return WPScriptValue::Number(0.0);
}

WPDynamicValue ToDynamicValue(const WPScriptValue& value, WPDynamicValue::Type type) {
    const auto number_at = [&](size_t index) {
        return index < value.numeric_values.size() ? value.numeric_values[index] : 0.0;
    };

    switch (type) {
    case WPDynamicValue::Type::Boolean:
        return WPDynamicValue(value.boolean_value);
    case WPDynamicValue::Type::String:
        return WPDynamicValue(value.string_value);
    case WPDynamicValue::Type::Int32:
        return WPDynamicValue(static_cast<int32_t>(number_at(0)));
    case WPDynamicValue::Type::UInt32:
        return WPDynamicValue(static_cast<uint32_t>(number_at(0)));
    case WPDynamicValue::Type::Float:
        return WPDynamicValue(static_cast<float>(number_at(0)));
    case WPDynamicValue::Type::Double:
        return WPDynamicValue(number_at(0));
    case WPDynamicValue::Type::Float2:
        return WPDynamicValue(
            std::array<float, 2> { static_cast<float>(number_at(0)), static_cast<float>(number_at(1)) });
    case WPDynamicValue::Type::Float3:
        return WPDynamicValue(std::array<float, 3> { static_cast<float>(number_at(0)),
                                                     static_cast<float>(number_at(1)),
                                                     static_cast<float>(number_at(2)) });
    case WPDynamicValue::Type::Float4:
        return WPDynamicValue(std::array<float, 4> { static_cast<float>(number_at(0)),
                                                     static_cast<float>(number_at(1)),
                                                     static_cast<float>(number_at(2)),
                                                     static_cast<float>(number_at(3)) });
    case WPDynamicValue::Type::FloatVector: {
        std::vector<float> values;
        values.reserve(value.numeric_values.size());
        for (const double component : value.numeric_values) {
            values.push_back(static_cast<float>(component));
        }
        return WPDynamicValue(std::move(values));
    }
    case WPDynamicValue::Type::Int3:
        return WPDynamicValue(std::array<int32_t, 3> { static_cast<int32_t>(number_at(0)),
                                                       static_cast<int32_t>(number_at(1)),
                                                       static_cast<int32_t>(number_at(2)) });
    case WPDynamicValue::Type::Null:
        break;
    }

    return WPDynamicValue {};
}

std::vector<std::string> ResolveVideoTextureKeys(const Scene& scene, const SceneNode* node) {
    std::vector<std::string> keys;
    if (node == nullptr || !node->HasMaterial()) {
        return keys;
    }

    auto* mutable_node = const_cast<SceneNode*>(node);
    auto* material = mutable_node->Mesh()->Material();
    if (material == nullptr) {
        return keys;
    }

    for (const auto& key : material->textures) {
        const auto texture_it = scene.textures.find(key);
        if (texture_it != scene.textures.end() && texture_it->second.isVideo &&
            std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
        }
    }

    return keys;
}

std::vector<WPScriptVideoTextureState> BuildVideoTextureStates(const Scene& scene,
                                                               const std::vector<std::string>& keys) {
    std::vector<WPScriptVideoTextureState> states;
    states.reserve(keys.size());
    for (const auto& key : keys) {
        const auto paused_it = scene.videoTexturePaused.find(key);
        states.push_back(WPScriptVideoTextureState {
            .key = key,
            .paused = paused_it != scene.videoTexturePaused.end() && paused_it->second,
            .stopped = scene.videoTextureStopped.count(key) != 0,
        });
    }
    return states;
}

void ApplyVideoTextureEvents(Scene& scene, const std::vector<WPScriptVideoTextureEvent>& events) {
    for (const auto& event : events) {
        if (event.key.empty()) {
            continue;
        }

        if (event.method == "play") {
            scene.videoTextureStopped.erase(event.key);
            scene.videoTexturePaused[event.key] = false;
        } else if (event.method == "pause") {
            scene.videoTextureStopped.erase(event.key);
            scene.videoTexturePaused[event.key] = true;
        } else if (event.method == "stop") {
            scene.videoTexturePaused[event.key] = true;
            scene.videoTextureStopped.insert(event.key);
        } else if (event.method == "setCurrentTime" && std::isfinite(event.current_time)) {
            scene.videoTextureSeekRequests[event.key] = std::max(0.0, event.current_time);
        }
    }
}

void RegisterSyntheticImage(Scene* scene, std::string_view key, std::shared_ptr<Image> image) {
    if (scene == nullptr || image == nullptr) return;
    auto* synthetic_parser = AsSyntheticImageParser(scene->imageParser.get());
    if (synthetic_parser == nullptr) return;
    synthetic_parser->RegisterImage(std::string(key), std::move(image));
}

void UpdateMediaThumbnailTextures(Scene* scene, const WPSceneScriptMediaState& media_state) {
    if (scene == nullptr) return;
    if (media_state.thumbnail_rgba.empty()) {
        RegisterSyntheticImage(scene,
                               WP_SCENE_SCRIPT_MEDIA_THUMBNAIL_TEXTURE,
                               CreateSceneScriptSolidImage(WP_SCENE_SCRIPT_MEDIA_THUMBNAIL_TEXTURE,
                                                           { 0, 0, 0, 0 }));
    } else {
        RegisterSyntheticImage(scene,
                               WP_SCENE_SCRIPT_MEDIA_THUMBNAIL_TEXTURE,
                               CreateSceneScriptRgbaImage(WP_SCENE_SCRIPT_MEDIA_THUMBNAIL_TEXTURE,
                                                          media_state.thumbnail_width,
                                                          media_state.thumbnail_height,
                                                          media_state.thumbnail_rgba));
    }

    if (media_state.previous_thumbnail_rgba.empty()) {
        RegisterSyntheticImage(
            scene,
            WP_SCENE_SCRIPT_MEDIA_PREVIOUS_THUMBNAIL_TEXTURE,
            CreateSceneScriptSolidImage(WP_SCENE_SCRIPT_MEDIA_PREVIOUS_THUMBNAIL_TEXTURE,
                                        { 0, 0, 0, 0 }));
    } else {
        RegisterSyntheticImage(
            scene,
            WP_SCENE_SCRIPT_MEDIA_PREVIOUS_THUMBNAIL_TEXTURE,
            CreateSceneScriptRgbaImage(WP_SCENE_SCRIPT_MEDIA_PREVIOUS_THUMBNAIL_TEXTURE,
                                       media_state.previous_thumbnail_width,
                                       media_state.previous_thumbnail_height,
                                       media_state.previous_thumbnail_rgba));
    }
}

bool MediaThumbnailChanged(const WPSceneScriptMediaState& lhs, const WPSceneScriptMediaState& rhs) {
    return lhs.has_thumbnail != rhs.has_thumbnail ||
           lhs.primary_color != rhs.primary_color ||
           lhs.secondary_color != rhs.secondary_color ||
           lhs.tertiary_color != rhs.tertiary_color ||
           lhs.text_color != rhs.text_color ||
           lhs.high_contrast_color != rhs.high_contrast_color ||
           lhs.thumbnail_width != rhs.thumbnail_width ||
           lhs.thumbnail_height != rhs.thumbnail_height ||
           lhs.thumbnail_rgba != rhs.thumbnail_rgba ||
           lhs.previous_thumbnail_width != rhs.previous_thumbnail_width ||
           lhs.previous_thumbnail_height != rhs.previous_thumbnail_height ||
           lhs.previous_thumbnail_rgba != rhs.previous_thumbnail_rgba;
}

bool MediaPropertiesChanged(const WPSceneScriptMediaState& lhs, const WPSceneScriptMediaState& rhs) {
    return lhs.title != rhs.title ||
           lhs.artist != rhs.artist ||
           lhs.album_title != rhs.album_title ||
           lhs.album_artist != rhs.album_artist ||
           lhs.sub_title != rhs.sub_title ||
           lhs.genres != rhs.genres ||
           lhs.content_type != rhs.content_type;
}

} // namespace

struct WPSceneScriptHost::Opaque {
#if WP_ENABLE_SCENESCRIPT_RUNTIME
    WPScriptRuntime runtime;
#endif
    bool initialized { false };
    std::vector<WPSceneScriptRegistration> binding_registrations;
    std::vector<WPSceneScriptRegistration> script_registrations;
    std::vector<AnimationInstance> animation_instances;
    std::unordered_map<std::string, size_t> animation_index_by_key;
    UserPropertyMap user_properties;
    std::unordered_map<std::string, std::string> general_settings;
    std::unordered_map<std::string, WPDynamicValue> resolved_values;
    WPSceneScriptMediaState media_state;
    WPSceneScriptMediaState dispatched_media_state;
    bool dispatch_media_thumbnail { false };
    bool dispatch_media_properties { false };
    bool dispatch_media_playback { false };
    size_t user_property_dispatch_count { 0 };
    size_t general_setting_dispatch_count { 0 };
    size_t media_dispatch_count { 0 };

    void ExecuteScriptRegistrations(Scene* scene);
};

void WPSceneScriptHost::Opaque::ExecuteScriptRegistrations(Scene* scene) {
#if WP_ENABLE_SCENESCRIPT_RUNTIME
    if (scene == nullptr || !runtime.isReady()) {
        return;
    }

    for (auto& registration : script_registrations) {
        if (!registration.setting.hasScript()) {
            continue;
        }

        const auto key = MakeRegistrationKey(registration);
        WPDynamicValue current_value = registration.base_value;
        if (const auto resolved_it = resolved_values.find(key); resolved_it != resolved_values.end()) {
            current_value = resolved_it->second;
        } else if (registration.setting.isDynamic()) {
            current_value = registration.setting.evaluate(&user_properties);
        }

        WPScriptEvaluationContext context;
        context.canvas_size = { static_cast<double>(scene->ortho[0]), static_cast<double>(scene->ortho[1]) };
        context.property_name = registration.property_name;

        for (const auto& [name, setting] : registration.setting.script_properties) {
            if (setting) {
                context.script_properties.emplace(name, ToScriptValue(setting->evaluate(&user_properties)));
            }
        }

        std::vector<WPScriptVideoTextureEvent> video_events;
        const auto video_keys = ResolveVideoTextureKeys(*scene, registration.node);
        context.video_textures = BuildVideoTextureStates(*scene, video_keys);
        context.video_texture_events = &video_events;
        context.media_state = &media_state;
        context.dispatch_media_thumbnail = dispatch_media_thumbnail;
        context.dispatch_media_properties = dispatch_media_properties;
        context.dispatch_media_playback = dispatch_media_playback;

        const auto evaluated =
            runtime.evaluate(registration.setting.script, ToScriptValue(current_value), context);
        ApplyVideoTextureEvents(*scene, video_events);

        if (evaluated.has_value()) {
            const WPDynamicValue::Type value_type = registration.value_type == WPDynamicValue::Type::Null
                ? current_value.type()
                : registration.value_type;
            resolved_values[key] = ToDynamicValue(*evaluated, value_type);
        }
    }
    dispatch_media_thumbnail = false;
    dispatch_media_properties = false;
    dispatch_media_playback = false;
#else
    (void)scene;
#endif
}

WPSceneScriptHost::WPSceneScriptHost(Scene* scene)
    : m_scene(scene)
    , m_impl(new Opaque()) {}

WPSceneScriptHost::~WPSceneScriptHost() {
    delete m_impl;
}

bool WPSceneScriptHost::Ready() const noexcept {
#if WP_ENABLE_SCENESCRIPT_RUNTIME
    return m_impl != nullptr && m_impl->runtime.isReady();
#else
    return false;
#endif
}

bool WPSceneScriptHost::RegisterPropertyBinding(WPSceneScriptRegistration registration) {
    if (!m_impl) {
        return false;
    }

    if (m_impl->initialized) {
        const auto resolved = registration.setting.evaluate(&m_impl->user_properties);
        m_impl->resolved_values[MakeRegistrationKey(registration)] = resolved;
    }

    m_impl->binding_registrations.push_back(std::move(registration));
    return true;
}

bool WPSceneScriptHost::RegisterPropertyScript(WPSceneScriptRegistration registration) {
    if (!m_impl) {
        return false;
    }

    if (m_impl->initialized) {
        const auto resolved = registration.setting.evaluate(&m_impl->user_properties);
        m_impl->resolved_values[MakeRegistrationKey(registration)] = resolved;
    }

    m_impl->script_registrations.push_back(std::move(registration));
    return true;
}

bool WPSceneScriptHost::RegisterPropertyAnimation(WPSceneScriptRegistration registration) {
    if (!m_impl || !registration.animation || !registration.animation->valid()) {
        return false;
    }

    AnimationInstance instance;
    instance.registration = std::move(registration);
    if (m_impl->initialized) {
        if (instance.registration.setting.isDynamic()) {
            instance.registration.base_value =
                instance.registration.setting.evaluate(&m_impl->user_properties);
        }
        InitializeAnimationInstance(instance);
        m_impl->resolved_values[MakeRegistrationKey(instance.registration)] =
            instance.registration.base_value;
    }

    const std::string key =
        MakeAnimationKey(instance.registration.object_id, instance.registration.property_name);
    m_impl->animation_index_by_key[key] = m_impl->animation_instances.size();
    m_impl->animation_instances.push_back(std::move(instance));
    return true;
}

void WPSceneScriptHost::Initialize() {
    if (!m_impl || m_impl->initialized) {
        return;
    }

    if (m_scene) {
        m_impl->user_properties = m_scene->userProperties;
    }

    for (auto& instance : m_impl->animation_instances) {
        if (instance.registration.setting.isDynamic()) {
            instance.registration.base_value =
                instance.registration.setting.evaluate(&m_impl->user_properties);
        }
        InitializeAnimationInstance(instance);
        m_impl->resolved_values[MakeRegistrationKey(instance.registration)] =
            instance.registration.base_value;
    }

    for (const auto& registration : m_impl->binding_registrations) {
        m_impl->resolved_values[MakeRegistrationKey(registration)] =
            registration.setting.evaluate(&m_impl->user_properties);
    }

    for (const auto& registration : m_impl->script_registrations) {
        m_impl->resolved_values[MakeRegistrationKey(registration)] =
            registration.setting.evaluate(&m_impl->user_properties);
    }

    m_impl->initialized = true;
    m_impl->ExecuteScriptRegistrations(m_scene);
}

void WPSceneScriptHost::MaterializeDeferredRuntimeLayersForResidency() {
    (void)m_scene;
}

void WPSceneScriptHost::FrameBegin(double frame_time) {
    if (!m_impl || !m_impl->initialized) {
        return;
    }

    for (auto& instance : m_impl->animation_instances) {
        if (!instance.registration.animation) {
            continue;
        }
        AdvancePropertyAnimationState(*instance.registration.animation, instance.state, frame_time);
    }

    m_impl->ExecuteScriptRegistrations(m_scene);
}

void WPSceneScriptHost::ApplyUserProperties(const UserPropertyMap& user_properties,
                                            bool initial_dispatch) {
    if (!m_impl) {
        return;
    }

    m_impl->user_properties = user_properties;
    if (m_scene) {
        m_scene->userProperties = user_properties;
    }

    auto apply_registration = [&](WPSceneScriptRegistration& registration) {
        if (!registration.setting.hasUserBinding()) {
            return;
        }

        const auto resolved = registration.setting.evaluate(&m_impl->user_properties);
        m_impl->resolved_values[MakeRegistrationKey(registration)] = resolved;
    };

    for (auto& registration : m_impl->binding_registrations) {
        apply_registration(registration);
    }

    for (auto& registration : m_impl->script_registrations) {
        apply_registration(registration);
    }

    for (auto& instance : m_impl->animation_instances) {
        if (!instance.registration.setting.hasUserBinding()) {
            continue;
        }

        instance.registration.base_value =
            instance.registration.setting.evaluate(&m_impl->user_properties);
        m_impl->resolved_values[MakeRegistrationKey(instance.registration)] =
            instance.registration.base_value;
    }

    if (!initial_dispatch || m_impl->user_property_dispatch_count == 0) {
        ++m_impl->user_property_dispatch_count;
    }

    m_impl->ExecuteScriptRegistrations(m_scene);
}

void WPSceneScriptHost::ApplyGeneralSettings(
    const std::unordered_map<std::string, std::string>& general_settings, bool initial_dispatch) {
    if (!m_impl) {
        return;
    }

    m_impl->general_settings = general_settings;
    if (!initial_dispatch || m_impl->general_setting_dispatch_count == 0) {
        ++m_impl->general_setting_dispatch_count;
    }
}

void WPSceneScriptHost::ApplyMediaState(const WPSceneScriptMediaState& media_state,
                                        bool initial_dispatch) {
    if (!m_impl) {
        return;
    }

    UpdateMediaThumbnailTextures(m_scene, media_state);
    const bool first_dispatch = m_impl->media_dispatch_count == 0;
    const bool thumbnail_changed =
        first_dispatch || MediaThumbnailChanged(m_impl->dispatched_media_state, media_state);
    const bool properties_changed =
        first_dispatch || MediaPropertiesChanged(m_impl->dispatched_media_state, media_state);
    const bool playback_changed =
        first_dispatch || m_impl->dispatched_media_state.playback_state != media_state.playback_state;

    m_impl->media_state = media_state;
    m_impl->dispatch_media_thumbnail = thumbnail_changed && (!initial_dispatch || first_dispatch);
    m_impl->dispatch_media_properties = properties_changed && (!initial_dispatch || first_dispatch);
    m_impl->dispatch_media_playback = playback_changed && (!initial_dispatch || first_dispatch);

    if (m_impl->dispatch_media_thumbnail ||
        m_impl->dispatch_media_properties ||
        m_impl->dispatch_media_playback) {
        ++m_impl->media_dispatch_count;
        m_impl->ExecuteScriptRegistrations(m_scene);
    }

    m_impl->dispatched_media_state = media_state;
}

size_t WPSceneScriptHost::BindingCount() const noexcept {
    return m_impl ? m_impl->binding_registrations.size() : 0;
}

size_t WPSceneScriptHost::ScriptCount() const noexcept {
    return m_impl ? m_impl->script_registrations.size() : 0;
}

size_t WPSceneScriptHost::AnimationCount() const noexcept {
    return m_impl ? m_impl->animation_instances.size() : 0;
}

size_t WPSceneScriptHost::UserPropertyDispatchCount() const noexcept {
    return m_impl ? m_impl->user_property_dispatch_count : 0;
}

size_t WPSceneScriptHost::GeneralSettingDispatchCount() const noexcept {
    return m_impl ? m_impl->general_setting_dispatch_count : 0;
}

size_t WPSceneScriptHost::MediaDispatchCount() const noexcept {
    return m_impl ? m_impl->media_dispatch_count : 0;
}

std::optional<WPDynamicValue> WPSceneScriptHost::FindResolvedValue(
    int32_t object_id, std::string_view property_name) const {
    if (!m_impl) {
        return std::nullopt;
    }

    const auto key = MakeAnimationKey(object_id, property_name);
    const auto it = m_impl->resolved_values.find(key);
    if (it == m_impl->resolved_values.end()) {
        return std::nullopt;
    }

    return it->second;
}

std::optional<WPPropertyAnimationState> WPSceneScriptHost::FindAnimationState(
    int32_t object_id, std::string_view property_name) const {
    if (!m_impl) {
        return std::nullopt;
    }

    const auto key = MakeAnimationKey(object_id, property_name);
    const auto it = m_impl->animation_index_by_key.find(key);
    if (it == m_impl->animation_index_by_key.end()) {
        return std::nullopt;
    }

    return m_impl->animation_instances[it->second].state;
}

std::optional<std::string> WPSceneScriptHost::FindGeneralSetting(std::string_view name) const {
    if (!m_impl) {
        return std::nullopt;
    }

    const auto it = m_impl->general_settings.find(std::string(name));
    if (it == m_impl->general_settings.end()) {
        return std::nullopt;
    }

    return it->second;
}

} // namespace wallpaper
