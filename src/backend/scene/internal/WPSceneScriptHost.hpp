#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

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
    void ApplyUserProperties(const UserPropertyMap& user_properties, bool initial_dispatch);
    void ApplyGeneralSettings(const std::unordered_map<std::string, std::string>& general_settings,
                              bool initial_dispatch);

    size_t BindingCount() const noexcept;
    size_t ScriptCount() const noexcept;
    size_t AnimationCount() const noexcept;
    size_t UserPropertyDispatchCount() const noexcept;
    size_t GeneralSettingDispatchCount() const noexcept;
    std::optional<WPDynamicValue> FindResolvedValue(
        int32_t object_id, std::string_view property_name) const;
    std::optional<WPPropertyAnimationState> FindAnimationState(
        int32_t object_id, std::string_view property_name) const;
    std::optional<std::string> FindGeneralSetting(std::string_view name) const;

private:
    Scene* m_scene { nullptr };
    Opaque* m_impl { nullptr };
};

} // namespace wallpaper
