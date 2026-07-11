#include "backend/scene/internal/settings/WPUserPropertiesJson.hpp"

#include <cassert>
#include <cmath>
#include <string>

namespace
{
const wallpaper::ShaderValue& AsShaderValue(const wallpaper::UserProperty& property) {
    const auto* value = std::get_if<wallpaper::ShaderValue>(&property.value);
    assert(value != nullptr);
    return *value;
}
} // namespace

int main() {
    auto parsed = wallpaper::ParseUserPropertiesJson(R"({
        "enabled": true,
        "scale": 1.5,
        "title": "clock",
        "color": [0.25, 0.5, 0.75]
    })");
    assert(parsed);
    assert(parsed.value().size() == 4);

    const auto& enabled = parsed.value().at("enabled");
    assert(enabled.is_boolean);
    assert(AsShaderValue(enabled).size() == 1);
    assert(std::abs(AsShaderValue(enabled)[0] - 1.0f) < 0.0001f);

    const auto& scale = parsed.value().at("scale");
    assert(! scale.is_boolean);
    assert(std::abs(AsShaderValue(scale)[0] - 1.5f) < 0.0001f);

    const auto* title = std::get_if<std::string>(&parsed.value().at("title").value);
    assert(title != nullptr && *title == "clock");

    const auto& color = AsShaderValue(parsed.value().at("color"));
    assert(color.size() == 3);
    assert(std::abs(color[2] - 0.75f) < 0.0001f);

    wallpaper::UserPropertyMap defaults;
    wallpaper::UserProperty defaultEnabled;
    defaultEnabled.value = wallpaper::ShaderValue(0.0f);
    defaultEnabled.condition = "master.value == 1";
    defaultEnabled.is_boolean = true;
    defaults.emplace("enabled", defaultEnabled);

    wallpaper::UserPropertyMap overrides;
    wallpaper::UserProperty overrideEnabled;
    overrideEnabled.value = wallpaper::ShaderValue(1.0f);
    overrides.emplace("enabled", overrideEnabled);

    auto merged = wallpaper::MergeUserPropertiesWithDefaults(defaults, overrides);
    assert(merged.size() == 1);
    assert(merged.at("enabled").condition == "master.value == 1");
    assert(merged.at("enabled").is_boolean);
    assert(std::abs(AsShaderValue(merged.at("enabled"))[0] - 1.0f) < 0.0001f);

    assert(! wallpaper::ParseUserPropertiesJson("[]"));
    assert(! wallpaper::ParseUserPropertiesJson(R"({"bad":null})"));
    assert(! wallpaper::ParseUserPropertiesJson(R"({"bad":[]})"));
    assert(! wallpaper::ParseUserPropertiesJson(R"({"bad":[1,"x"]})"));

    return 0;
}
