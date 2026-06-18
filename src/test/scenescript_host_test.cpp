#include "backend/scene/internal/WPSceneScriptHost.hpp"
#include "backend/scene/internal/WPSceneParser.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "backend/scene/internal/scene/include/scene/Scene.h"

namespace
{

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

bool NearlyEqual(double lhs, double rhs, double epsilon = 0.0001) {
    return std::abs(lhs - rhs) <= epsilon;
}

std::shared_ptr<wallpaper::WPPropertyAnimationDefinition> MakeAnimationDefinition() {
    auto definition = std::make_shared<wallpaper::WPPropertyAnimationDefinition>();
    definition->channel_count = 1;
    definition->fps = 10.0;
    definition->frame_count = 10.0;
    definition->mode = wallpaper::WPPropertyAnimationMode::Single;

    wallpaper::WPPropertyAnimationKeyframe start;
    start.frame = 0.0;
    start.value = 0.0;

    wallpaper::WPPropertyAnimationKeyframe end;
    end.frame = 10.0;
    end.value = 1.0;

    definition->channels[0].keyframes = { start, end };
    return definition;
}

wallpaper::WPSceneScriptRegistration MakeRegistration(
    int32_t object_id,
    std::string object_name,
    std::string property_name,
    wallpaper::WPSceneScriptTargetKind target_kind,
    wallpaper::WPDynamicValue::Type value_type,
    wallpaper::WPDynamicValue base_value) {
    wallpaper::WPSceneScriptRegistration registration;
    registration.object_id = object_id;
    registration.object_name = std::move(object_name);
    registration.property_name = std::move(property_name);
    registration.target_kind = target_kind;
    registration.value_type = value_type;
    registration.base_value = std::move(base_value);
    registration.setting.value = registration.base_value;
    return registration;
}

void BindToUserProperty(wallpaper::WPSceneScriptRegistration& registration, std::string name) {
    wallpaper::UserPropertyBinding binding;
    binding.name = std::move(name);
    registration.setting.property = std::move(binding);
}

} // namespace

int main() {
    using wallpaper::Scene;
    using wallpaper::ShaderValue;
    using wallpaper::UserProperty;
    using wallpaper::UserPropertyMap;
    using wallpaper::WPDynamicValue;
    using wallpaper::WPSceneScriptHost;
    using wallpaper::WPSceneScriptRegistration;
    using wallpaper::WPSceneScriptTargetKind;

    Scene scene;
    WPSceneScriptHost host(&scene);

    Require(host.Ready(), "scenescript host should be ready when QuickJS runtime is enabled");

    Require(host.RegisterPropertyBinding(MakeRegistration(1,
                                                          "LayerA",
                                                          "alpha",
                                                          WPSceneScriptTargetKind::Layer,
                                                          WPDynamicValue::Type::Float,
                                                          WPDynamicValue(1.0f))),
            "property binding registration should succeed");
    Require(host.RegisterPropertyScript(MakeRegistration(
                2,
                "LayerB",
                "origin",
                WPSceneScriptTargetKind::Layer,
                WPDynamicValue::Type::Float3,
                WPDynamicValue(std::array<float, 3> { 0.0f, 0.0f, 0.0f }))),
            "property script registration should succeed");

    auto animated_registration = MakeRegistration(3,
                                                  "LayerC",
                                                  "alpha",
                                                  WPSceneScriptTargetKind::Layer,
                                                  WPDynamicValue::Type::Float,
                                                  WPDynamicValue(0.0f));
    animated_registration.animation = MakeAnimationDefinition();
    Require(host.RegisterPropertyAnimation(std::move(animated_registration)),
            "property animation registration should succeed");

    Require(host.BindingCount() == 1, "host should track binding registrations");
    Require(host.ScriptCount() == 1, "host should track script registrations");
    Require(host.AnimationCount() == 1, "host should track animation registrations");

    host.Initialize();
    auto resolved_alpha = host.FindResolvedValue(1, "alpha");
    Require(resolved_alpha.has_value(), "initialized host should expose binding value");
    float alpha = 0.0f;
    Require(resolved_alpha->tryGet(&alpha), "resolved alpha should be a float");
    Require(NearlyEqual(alpha, 1.0), "resolved alpha should start with authored value");

    const auto initial_state = host.FindAnimationState(3, "alpha");
    Require(initial_state.has_value(), "initialized host should expose animation state");
    Require(initial_state->playing, "initialized animation should start playing");
    Require(NearlyEqual(initial_state->frame, 0.0), "initialized animation frame should start at zero");

    host.FrameBegin(0.5);
    const auto advanced_state = host.FindAnimationState(3, "alpha");
    Require(advanced_state.has_value(), "advanced host should retain animation state");
    Require(NearlyEqual(advanced_state->frame, 5.0), "frame begin should advance animation state");

    auto late_registration = MakeRegistration(4,
                                              "LayerD",
                                              "opacity",
                                              WPSceneScriptTargetKind::Layer,
                                              WPDynamicValue::Type::Float,
                                              WPDynamicValue(0.0f));
    late_registration.animation = MakeAnimationDefinition();
    Require(host.RegisterPropertyAnimation(std::move(late_registration)),
            "late animation registration should succeed");
    const auto late_state = host.FindAnimationState(4, "opacity");
    Require(late_state.has_value(), "late animation registration should initialize state immediately");
    Require(late_state->playing, "late animation should start playing");

    auto bound_registration = MakeRegistration(5,
                                               "LayerE",
                                               "alpha",
                                               WPSceneScriptTargetKind::Layer,
                                               WPDynamicValue::Type::Float,
                                               WPDynamicValue(0.25f));
    BindToUserProperty(bound_registration, "layer_alpha");
    Require(host.RegisterPropertyBinding(std::move(bound_registration)),
            "late user-bound property registration should succeed");

    auto bound_animation = MakeRegistration(6,
                                            "LayerF",
                                            "alpha",
                                            WPSceneScriptTargetKind::Layer,
                                            WPDynamicValue::Type::Float,
                                            WPDynamicValue(0.0f));
    BindToUserProperty(bound_animation, "animated_alpha");
    bound_animation.animation = MakeAnimationDefinition();
    Require(host.RegisterPropertyAnimation(std::move(bound_animation)),
            "user-bound animation registration should succeed");

    UserPropertyMap properties;
    properties.emplace(
        "layer_alpha",
        UserProperty { .value = ShaderValue(0.75f), .condition = {}, .is_boolean = false });
    properties.emplace(
        "animated_alpha",
        UserProperty { .value = ShaderValue(0.5f), .condition = {}, .is_boolean = false });

    host.ApplyUserProperties(properties, false);
    Require(host.UserPropertyDispatchCount() == 1, "user property dispatch should be counted");
    Require(scene.userProperties.size() == 2, "scene should retain latest user properties");

    const auto user_alpha_value = host.FindResolvedValue(5, "alpha");
    Require(user_alpha_value.has_value(), "user binding should resolve a value");
    alpha = 0.0f;
    Require(user_alpha_value->tryGet(&alpha), "user binding value should be a float");
    Require(NearlyEqual(alpha, 0.75), "user binding should use latest user property");

    const auto animated_alpha_value = host.FindResolvedValue(6, "alpha");
    Require(animated_alpha_value.has_value(), "user-bound animation should resolve base value");
    alpha = 0.0f;
    Require(animated_alpha_value->tryGet(&alpha), "animation base value should be a float");
    Require(NearlyEqual(alpha, 0.5), "animation base value should use latest user property");

    host.ApplyGeneralSettings({ { "language", "zh-cn" }, { "quality", "high" } }, true);
    Require(host.GeneralSettingDispatchCount() == 1, "initial general setting dispatch should be counted");
    host.ApplyGeneralSettings({ { "language", "en-us" } }, false);
    Require(host.GeneralSettingDispatchCount() == 2, "general setting updates should be counted");
    const auto language = host.FindGeneralSetting("language");
    Require(language.has_value(), "general setting should be queryable");
    Require(*language == "en-us", "general setting should retain latest value");

    Scene parsed_scene;
    wallpaper::SceneNode parsed_node;
    RegisterSceneScriptBindingsForTest(
        parsed_scene,
        nlohmann::json {
            { "general",
              {
                  { "clearcolor",
                    { { "value", nlohmann::json::array({ 0.1f, 0.2f, 0.3f }) },
                      { "user", "scene_color" } } },
              } },
            { "objects",
              nlohmann::json::array({
                  {
                      { "id", 7 },
                      { "name", "ScriptedLayer" },
                      { "alpha", { { "value", 0.4f }, { "user", "layer_alpha" } } },
                      { "origin",
                        { { "value", nlohmann::json::array({ 1.0f, 2.0f, 3.0f }) },
                          { "script", "return value;" } } },
                      { "brightness",
                        { { "value", 0.0f },
                          { "animation",
                            {
                                { "options", { { "fps", 10.0 }, { "length", 10.0 } } },
                                { "c0",
                                  nlohmann::json::array({
                                      { { "frame", 0.0 }, { "value", 0.0 } },
                                      { { "frame", 10.0 }, { "value", 1.0 } },
                                  }) },
                            } } } },
                  },
              }) },
        },
        &parsed_node);
    Require(parsed_scene.bindingRegistrations.size() == 2,
            "parser should register layer and general user bindings");
    Require(parsed_scene.scriptRegistrations.size() == 1,
            "parser should register script bindings");
    Require(parsed_scene.propertyAnimationRegistrations.size() == 1,
            "parser should register property animations");
    Require(parsed_scene.bindingRegistrations[0].property_name == "clearcolor",
            "general binding should preserve property name");
    Require(parsed_scene.bindingRegistrations[1].property_name == "alpha",
            "layer binding should preserve property name");
    Require(parsed_scene.scriptRegistrations[0].node == &parsed_node,
            "script registration should bind parsed scene node");
    Require(parsed_scene.propertyAnimationRegistrations[0].animation != nullptr,
            "property animation registration should own animation definition");

    return 0;
}
