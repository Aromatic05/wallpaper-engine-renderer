#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "WPSceneScriptRegistration.hpp"

namespace wallpaper
{

class Scene;

class WPSceneScriptHost {
public:
    struct Opaque;

    explicit WPSceneScriptHost(Scene* scene);
    ~WPSceneScriptHost();

    WPSceneScriptHost(const WPSceneScriptHost&) = delete;
    WPSceneScriptHost& operator=(const WPSceneScriptHost&) = delete;

    bool Ready() const noexcept;

    bool RegisterPropertyBinding(WPSceneScriptRegistration registration);
    bool RegisterPropertyScript(WPSceneScriptRegistration registration);
    bool RegisterPropertyAnimation(WPSceneScriptRegistration registration);
    void Initialize();
    void MaterializeDeferredRuntimeLayersForResidency();
    void FrameBegin(double frame_time);

    size_t BindingCount() const noexcept;
    size_t ScriptCount() const noexcept;
    size_t AnimationCount() const noexcept;
    std::optional<WPPropertyAnimationState> FindAnimationState(
        int32_t object_id, std::string_view property_name) const;

private:
    Scene* m_scene { nullptr };
    Opaque* m_impl { nullptr };
};

} // namespace wallpaper
