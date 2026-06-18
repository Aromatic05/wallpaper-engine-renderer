#include "backend/scene/internal/WPPropertyAnimation.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{

void Require(bool condition, const char* message) {
    if (! condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

bool NearlyEqual(double lhs, double rhs, double epsilon = 0.0001) {
    return std::abs(lhs - rhs) <= epsilon;
}

} // namespace

int main() {
    using wallpaper::AdvancePropertyAnimationState;
    using wallpaper::EvaluatePropertyAnimation;
    using wallpaper::InitializePropertyAnimationState;
    using wallpaper::ParsePropertyAnimationDefinition;
    using wallpaper::WPDynamicValue;
    using wallpaper::WPPropertyAnimationDefinition;
    using wallpaper::WPPropertyAnimationMode;
    using wallpaper::WPPropertyAnimationState;

    {
        WPPropertyAnimationDefinition definition;
        const auto json = nlohmann::json {
            { "animation",
              { { "options",
                  { { "fps", 10.0 }, { "length", 10.0 }, { "mode", "single" }, { "name", "Fade" } } },
                { "c0",
                  nlohmann::json::array(
                      { nlohmann::json { { "frame", 0.0 }, { "value", 0.0 } },
                        nlohmann::json { { "frame", 10.0 }, { "value", 1.0 } } }) } } }
        };
        Require(ParsePropertyAnimationDefinition(json, WPDynamicValue::Type::Float, definition),
                "single-channel animation should parse");
        Require(definition.valid(), "parsed animation should be valid");
        Require(definition.channel_count == 1, "float animation should have one channel");
        Require(definition.mode == WPPropertyAnimationMode::Single, "mode should parse");
        Require(definition.name == "Fade", "name should parse");
    }

    {
        WPPropertyAnimationDefinition definition;
        const auto json = nlohmann::json {
            { "animation",
              { { "options", { { "fps", 10.0 }, { "length", 10.0 }, { "mode", "single" } } },
                { "c0",
                  nlohmann::json::array(
                      { nlohmann::json { { "frame", 0.0 }, { "value", 0.0 } },
                        nlohmann::json { { "frame", 10.0 }, { "value", 1.0 } } }) } } }
        };
        Require(ParsePropertyAnimationDefinition(json, WPDynamicValue::Type::Float, definition),
                "linear animation should parse");

        WPPropertyAnimationState state;
        InitializePropertyAnimationState(definition, state);
        Require(state.playing, "non-paused animation should start playing");

        const bool hit_boundary = AdvancePropertyAnimationState(definition, state, 0.5);
        Require(! hit_boundary, "half-second step should not stop single animation");
        Require(NearlyEqual(state.frame, 5.0), "frame should advance by fps * dt");

        const auto value =
            EvaluatePropertyAnimation(definition, state, WPDynamicValue(0.0f), WPDynamicValue::Type::Float);
        Require(value.has_value(), "float animation should evaluate");

        float scalar = 0.0f;
        Require(value->tryGet(&scalar), "evaluated animation should return float");
        Require(NearlyEqual(scalar, 0.5), "linear interpolation should hit midpoint");
    }

    {
        WPPropertyAnimationDefinition definition;
        const auto json = nlohmann::json {
            { "animation",
              { { "options", { { "fps", 10.0 }, { "length", 10.0 }, { "mode", "loop" } } },
                { "relative", true },
                { "c0",
                  nlohmann::json::array(
                      { nlohmann::json { { "frame", 0.0 }, { "value", 0.0 } },
                        nlohmann::json { { "frame", 10.0 }, { "value", 2.0 } } }) },
                { "c1",
                  nlohmann::json::array(
                      { nlohmann::json { { "frame", 0.0 }, { "value", 0.0 } },
                        nlohmann::json { { "frame", 10.0 }, { "value", 4.0 } } }) } } }
        };
        Require(ParsePropertyAnimationDefinition(json, WPDynamicValue::Type::Float2, definition),
                "float2 animation should parse");

        WPPropertyAnimationState state;
        InitializePropertyAnimationState(definition, state);
        AdvancePropertyAnimationState(definition, state, 0.5);

        const auto value = EvaluatePropertyAnimation(
            definition,
            state,
            WPDynamicValue(std::array<float, 2> { 10.0f, 20.0f }),
            WPDynamicValue::Type::Float2);
        Require(value.has_value(), "relative float2 animation should evaluate");

        std::array<float, 2> vector {};
        Require(value->tryGet(&vector), "evaluated animation should return float2");
        Require(NearlyEqual(vector[0], 11.0), "relative first channel should offset base");
        Require(NearlyEqual(vector[1], 22.0), "relative second channel should offset base");
    }

    {
        WPPropertyAnimationDefinition definition;
        const auto json = nlohmann::json {
            { "animation",
              { { "options", { { "fps", 10.0 }, { "length", 10.0 }, { "mode", "single" } } },
                { "c0",
                  nlohmann::json::array(
                      { nlohmann::json { { "frame", 0.0 }, { "value", 0.0 } },
                        nlohmann::json { { "frame", 10.0 }, { "value", 1.0 } } }) } } }
        };
        Require(ParsePropertyAnimationDefinition(json, WPDynamicValue::Type::Boolean, definition),
                "boolean animation should parse");

        WPPropertyAnimationState state;
        InitializePropertyAnimationState(definition, state);
        AdvancePropertyAnimationState(definition, state, 0.6);

        const auto value =
            EvaluatePropertyAnimation(definition, state, WPDynamicValue(false), WPDynamicValue::Type::Boolean);
        Require(value.has_value(), "boolean animation should evaluate");
        bool enabled = false;
        Require(value->tryGet(&enabled), "boolean animation should return bool");
        Require(enabled, "boolean animation should threshold at >= 0.5");
    }

    return 0;
}
