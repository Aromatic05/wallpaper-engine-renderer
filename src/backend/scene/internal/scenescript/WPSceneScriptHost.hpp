#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "interface/IShaderValueUpdater.h"
#include "WPPropertyAnimation.hpp"
#include "WPUserSetting.hpp"

namespace wallpaper
{

class Scene;
class SceneNode;
struct WPSceneScriptMediaState;

enum class WPSceneScriptTargetKind
{
    // Scene targets cover global `general` properties. They intentionally do not carry a node
    // pointer because values such as camera parallax affect shader state for every layer.
    Scene,
    // Sound targets are runtime audio streams rather than SceneNode-owned visual layers. They need
    // their own dispatch kind so user-bound volume changes can update the mounted SoundManager
    // channel even though the sound object has no mesh node in the scene graph.
    Sound,
    // Camera targets are authored camera layers. They own a SceneNode like regular empty layers,
    // but their origin/zoom/visibility properties must update the active SceneCamera instead of
    // only moving an invisible transform container.
    Camera,
    Layer,
    AnimationLayer,
    Effect,
    // MaterialUniform targets are shader-level user bindings declared by material
    // `usershadervalues`. The parser resolves the authored material-value alias to a concrete GLSL
    // uniform once, then live user-property updates reuse that registration without embedding any
    // project-specific property names in the runtime dispatcher.
    // They are separate from ordinary layer `color` because stock shaders can expose many custom
    // color uniforms that are not represented by the generic layer color fields.
    MaterialUniform,
};

struct WPSceneScriptRegistration {
    int32_t                                     object_id { 0 };
    std::string                                 object_name;
    std::string                                 property_name;
    SceneNode*                                  node { nullptr };
    WPSceneScriptTargetKind                     target_kind { WPSceneScriptTargetKind::Layer };
    uint32_t                                    target_index { 0 };
    int32_t                                     target_id { 0 };
    uint32_t                                    target_effect_index { 0 };
    std::string                                 authored_property_name;
    WPDynamicValue::Type                        value_type { WPDynamicValue::Type::Null };
    WPDynamicValue                              base_value {};
    std::shared_ptr<WPPropertyAnimationDefinition> animation;
    WPUserSetting                               setting;
};

class WPSceneScriptHost {
public:
    struct Opaque;

    explicit WPSceneScriptHost(Scene* scene);
    ~WPSceneScriptHost();

    WPSceneScriptHost(const WPSceneScriptHost&)            = delete;
    WPSceneScriptHost& operator=(const WPSceneScriptHost&) = delete;

    bool Ready() const noexcept;

    bool RegisterPropertyBinding(WPSceneScriptRegistration registration);
    bool RegisterPropertyScript(WPSceneScriptRegistration registration);
    bool RegisterPropertyAnimation(WPSceneScriptRegistration registration);
    void Initialize();
    // Materialize at most one deferred hidden layer after a presented frame. Keeping this work
    // incremental removes hidden-language/resource branches from the startup critical path.
    bool MaterializeNextDeferredRuntimeLayerForResidency();
    void FrameBegin(double frame_time);
    void ApplyUserProperties(const UserPropertyMap& user_properties, bool initial_dispatch);
    void ApplyGeneralSettings(const std::unordered_map<std::string, std::string>& general_settings,
                              bool initial_dispatch);
    void ApplyAudioSamples(const std::vector<float>& audio_samples);
    bool GetAudioSpectrum(uint32_t resolution,
                          std::vector<float>* left,
                          std::vector<float>* right,
                          std::vector<float>* average) const;
    void ApplyMediaState(const WPSceneScriptMediaState& media_state, bool initial_dispatch = false);
    void HandleCursorMove();
    void HandleCursorButton(bool down);
    void ResizeScreen(int32_t width, int32_t height);
    void ApplyTextureAnimations(SceneNode* node, sprite_map_t& sprites, double frame_time);
    void NotifyAnimationLayersAdvanced(SceneNode* node);

private:
    Scene*  m_scene { nullptr };
    Opaque* m_impl { nullptr };
};

} // namespace wallpaper
