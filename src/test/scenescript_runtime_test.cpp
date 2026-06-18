#include "backend/scene/internal/WPScriptRuntime.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

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

    return 0;
}
