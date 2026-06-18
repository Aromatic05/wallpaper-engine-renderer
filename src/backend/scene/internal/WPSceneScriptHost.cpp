#include "WPSceneScriptHost.hpp"

#include <utility>

#include "scene/Scene.h"

#if WP_ENABLE_SCENESCRIPT_RUNTIME
#include "WPScriptRuntime.hpp"
#endif

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

void InitializeAnimationInstance(AnimationInstance& instance) {
    if (!instance.registration.animation) {
        return;
    }

    InitializePropertyAnimationState(*instance.registration.animation, instance.state);
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
};

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

    m_impl->binding_registrations.push_back(std::move(registration));
    return true;
}

bool WPSceneScriptHost::RegisterPropertyScript(WPSceneScriptRegistration registration) {
    if (!m_impl) {
        return false;
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
        InitializeAnimationInstance(instance);
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

    for (auto& instance : m_impl->animation_instances) {
        InitializeAnimationInstance(instance);
    }

    m_impl->initialized = true;
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

} // namespace wallpaper
