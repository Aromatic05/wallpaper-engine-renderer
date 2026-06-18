#include "backend/scene/internal/WPSceneScriptHost.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

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
    return registration;
}

} // namespace

int main() {
    using wallpaper::Scene;
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

    return 0;
}
