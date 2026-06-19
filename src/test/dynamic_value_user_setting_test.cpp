#include "backend/scene/internal/settings/WPDynamicValue.hpp"
#include "backend/scene/internal/settings/WPUserSetting.hpp"

#include <nlohmann/json.hpp>

#include <array>
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

} // namespace

int main() {
    using wallpaper::ShaderValue;
    using wallpaper::UserProperty;
    using wallpaper::UserPropertyMap;
    using wallpaper::WPDynamicValue;
    using wallpaper::WPUserSetting;

    {
        const auto parsed = WPDynamicValue::FromJsonLiteral(nlohmann::json::array({ 1.0f, 2.0f }),
                                                            WPDynamicValue::Type::Float2);
        Require(parsed.has_value(), "float2 json should parse");

        std::array<float, 2> value {};
        Require(parsed->tryGet(&value), "float2 should round-trip");
        Require(value[0] == 1.0f && value[1] == 2.0f, "float2 values should match");
    }

    {
        const wallpaper::UserPropertyValue property = ShaderValue(std::array<float, 3> { 1.0f, 2.0f, 3.0f });
        const auto converted =
            WPDynamicValue::FromUserPropertyValue(property, WPDynamicValue::Type::Float3);
        Require(converted.has_value(), "shader value should convert to float3");

        std::array<float, 3> value {};
        Require(converted->tryGet(&value), "converted float3 should be readable");
        Require(value[2] == 3.0f, "converted float3 tail should match");
    }

    {
        WPUserSetting setting;
        Require(ParseUserSetting(nlohmann::json({ { "value", 3.5f } }),
                                 setting,
                                 WPDynamicValue::Type::Float),
                "plain value setting should parse");
        float value = 0.0f;
        Require(setting.evaluateAs(&value, nullptr), "plain value should evaluate");
        Require(value == 3.5f, "plain value should stay authored");
    }

    {
        WPUserSetting setting;
        Require(ParseUserSetting(nlohmann::json({ { "value", false }, { "user", "enabled" } }),
                                 setting,
                                 WPDynamicValue::Type::Boolean),
                "user binding setting should parse");

        UserPropertyMap properties;
        properties.emplace("enabled", UserProperty { .value = ShaderValue(1.0f), .condition = {}, .is_boolean = true });

        bool value = false;
        Require(setting.evaluateAs(&value, &properties), "bound boolean should evaluate");
        Require(value, "user binding should override authored value");
    }

    {
        WPUserSetting setting;
        Require(ParseUserSetting(
                    nlohmann::json({ { "value", true },
                                     { "user", { { "name", "mode" }, { "condition", "0" } } } }),
                    setting,
                    WPDynamicValue::Type::Boolean),
                "conditional binding should parse");

        UserPropertyMap properties;
        properties.emplace("mode", UserProperty { .value = std::string("1"), .condition = {}, .is_boolean = false });

        bool value = true;
        Require(setting.evaluateAs(&value, &properties), "conditional binding should evaluate");
        Require(! value, "inactive conditional branch should collapse to neutral value");
    }

    return 0;
}
