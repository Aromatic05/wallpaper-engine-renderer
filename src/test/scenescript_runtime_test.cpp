#include "backend/scene/internal/scenescript/WPScriptRuntime.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

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

} // namespace

int main() {
    using wallpaper::WPScriptEvaluationContext;
    using wallpaper::WPScriptRuntime;
    using wallpaper::WPScriptValue;

    WPScriptRuntime runtime;
    Require(runtime.isReady(), "QuickJS runtime should initialize");

    {
        WPScriptEvaluationContext context;
        context.property_name = "alpha";
        context.canvas_size = { 1920.0, 1080.0 };
        context.script_properties.emplace("gain", WPScriptValue::Number(1.5));

        const auto result = runtime.evaluate(R"(
            import { ignored } from "workshop";
            export function update(value) {
              return value + engine.canvasSize.x / 1920 + shared.bump + __scriptProps.gain;
            }
            shared.bump = 0.5;
        )",
                                             WPScriptValue::Number(2.0),
                                             context);
        Require(result.has_value(), "numeric scenescript should evaluate");
        Require(result->shape == wallpaper::WPScriptValueShape::Number, "numeric result shape should match");
        Require(result->numeric_values.size() == 1, "numeric result should contain one number");
        Require(NearlyEqual(result->numeric_values[0], 5.0), "numeric script result should match");
    }

    {
        WPScriptEvaluationContext context;
        context.property_name = "visible";

        const auto result = runtime.evaluate(R"(
            "use strict";
            export function update(value) {
              return !value;
            }
        )",
                                             WPScriptValue::Boolean(true),
                                             context);
        Require(result.has_value(), "boolean scenescript should evaluate");
        Require(result->shape == wallpaper::WPScriptValueShape::Boolean, "boolean result shape should match");
        Require(!result->boolean_value, "boolean script should negate current value");
    }

    {
        WPScriptEvaluationContext context;
        context.property_name = "caption";

        const auto result = runtime.evaluate(R"(
            function update(value) {
              return `${value}-ready`;
            }
        )",
                                             WPScriptValue::String("scene"),
                                             context);
        Require(result.has_value(), "string scenescript should evaluate");
        Require(result->shape == wallpaper::WPScriptValueShape::String, "string result shape should match");
        Require(result->string_value == "scene-ready", "string script result should match");
    }

    {
        WPScriptEvaluationContext context;
        context.property_name = "origin";

        const auto result = runtime.evaluate(R"(
            function update(value) {
              return new Vec3(value).add(new Vec3(1, 2, 3));
            }
        )",
                                             WPScriptValue::VectorString({ 2.0, 4.0, 6.0 }),
                                             context);
        Require(result.has_value(), "vector scenescript should evaluate");
        Require(result->shape == wallpaper::WPScriptValueShape::VectorString, "vector result shape should match");
        Require(result->numeric_values.size() == 3, "vector result should contain three numbers");
        Require(NearlyEqual(result->numeric_values[0], 3.0), "vector x should match");
        Require(NearlyEqual(result->numeric_values[1], 6.0), "vector y should match");
        Require(NearlyEqual(result->numeric_values[2], 9.0), "vector z should match");
    }

    {
        WPScriptEvaluationContext context;
        context.property_name = "alpha";

        const auto result = runtime.evaluate(R"(
            function update(value) {
              return { bad: true };
            }
        )",
                                             WPScriptValue::Number(1.0),
                                             context);
        Require(!result.has_value(), "unsupported return type should fail conversion");
    }

    {
        WPScriptEvaluationContext context;
        context.property_name = "alpha";

        const auto first = runtime.evaluate(R"(
            if (!shared.queued) {
              shared.queued = true;
              shared.value = 1;
              engine.setTimeout(() => { shared.value = 4; }, 0);
            }
            function update(value) {
              return value + shared.value;
            }
        )",
                                            WPScriptValue::Number(1.0),
                                            context);
        Require(first.has_value(), "timer setup script should evaluate");
        Require(NearlyEqual(first->numeric_values[0], 2.0),
                "timer callback should not run in the same dispatch slice");

        const auto second = runtime.evaluate(R"(
            function update(value) {
              return value + shared.value;
            }
        )",
                                             WPScriptValue::Number(1.0),
                                             context);
        Require(second.has_value(), "timer dispatch script should evaluate");
        Require(NearlyEqual(second->numeric_values[0], 5.0),
                "zero-delay timer should run on the next runtime dispatch");
    }

    {
        WPScriptEvaluationContext context;
        context.property_name = "alpha";
        context.scene_layers = {
            { .id = 7, .name = "Back", .initial_config_json = R"({"id":7,"name":"Back"})" },
            { .id = 9, .name = "Front", .initial_config_json = R"({"id":9,"name":"Front"})" },
        };
        std::vector<wallpaper::WPScriptLayerEvent> layer_events;
        context.layer_events = &layer_events;

        const auto result = runtime.evaluate(R"(
            function update(value) {
              const back = thisScene.getLayer('Back');
              if (!back || back.getLayerIndex() !== 0) return 0;
              if (thisScene.getLayerCount() !== 2) return 0;
              if (thisScene.getInitialLayerConfig(back).name !== 'Back') return 0;
              thisScene.sortLayer(back, 1);
              thisScene.destroyLayer('Front');
              thisScene.createLayer({ name: 'DynamicLayer', visible: true });
              return value + 1;
            }
        )",
                                             WPScriptValue::Number(4.0),
                                             context);
        Require(result.has_value(), "layer scenescript should evaluate");
        Require(NearlyEqual(result->numeric_values[0], 5.0), "layer script result should match");
        Require(layer_events.size() == 3, "layer script should emit create/sort/destroy events");
        Require(layer_events[0].method == "sort" && layer_events[0].layer_id == 7 &&
                    layer_events[0].target_index == 1,
                "sort layer event should preserve layer id and target index");
        Require(layer_events[1].method == "destroy" && layer_events[1].layer_id == 9,
                "destroy layer event should resolve layer names");
        Require(layer_events[2].method == "create" && layer_events[2].name == "DynamicLayer",
                "create layer event should preserve config name");
    }

    return 0;
}
