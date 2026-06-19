#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "animation/WPPropertyAnimation.hpp"
#include "settings/WPUserSetting.hpp"

namespace wallpaper
{

class SceneNode;

enum class WPSceneScriptTargetKind
{
    Scene,
    Sound,
    Camera,
    Layer,
    AnimationLayer,
    Effect,
    MaterialUniform,
};

struct WPSceneScriptRegistration {
    int32_t object_id { 0 };
    std::string object_name;
    std::string property_name;
    SceneNode* node { nullptr };
    WPSceneScriptTargetKind target_kind { WPSceneScriptTargetKind::Layer };
    uint32_t target_index { 0 };
    int32_t target_id { 0 };
    WPDynamicValue::Type value_type { WPDynamicValue::Type::Null };
    WPDynamicValue base_value {};
    std::shared_ptr<WPPropertyAnimationDefinition> animation;
    WPUserSetting setting;
};

} // namespace wallpaper
