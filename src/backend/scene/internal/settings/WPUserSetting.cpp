#include "WPUserSetting.hpp"

#include <nlohmann/json.hpp>

#include "WPJson.hpp"

namespace wallpaper
{
namespace
{

std::optional<UserPropertyBinding> ParseUserBinding(const nlohmann::json& json) {
    if (! json.is_object() || ! json.contains("user") || json.at("user").is_null()) {
        return std::nullopt;
    }

    UserPropertyBinding binding;
    const auto& user = json.at("user");
    if (user.is_string()) {
        binding.name = user.get<std::string>();
    } else if (user.is_object()) {
        if (user.contains("name") && user.at("name").is_string()) {
            binding.name = user.at("name").get<std::string>();
        }
        if (user.contains("condition") && user.at("condition").is_string()) {
            binding.condition = user.at("condition").get<std::string>();
        }
    }

    if (binding.empty()) return std::nullopt;
    return binding;
}

const nlohmann::json* ResolveInitialValueNode(const nlohmann::json& json) {
    if (! json.is_object()) return &json;
    if (json.contains("value")) {
        const auto& value = json.at("value");
        // Script-property bindings can nest another conditional user binding in their authored
        // value. Wallpaper Engine still exposes the inner authored payload during init(), then
        // applies the actual user selection later through applyUserProperties(), so unwrap the
        // nested "value" chain instead of treating the binding object itself as a runtime value.
        if (value.is_object() && value.contains("value")) return ResolveInitialValueNode(value);
        return &value;
    }

    if (json.contains("animation") && json.at("animation").is_object()) {
        const auto& animation = json.at("animation");
        bool        start_paused { false };
        if (animation.contains("options") && animation.at("options").is_object() &&
            animation.at("options").contains("startpaused") &&
            animation.at("options").at("startpaused").is_boolean()) {
            start_paused = animation.at("options").at("startpaused").get<bool>();
        }

        if (start_paused && animation.contains("c0") && animation.at("c0").is_array() &&
            ! animation.at("c0").empty() && animation.at("c0").front().is_object() &&
            animation.at("c0").front().contains("value")) {
            return &animation.at("c0").front().at("value");
        }
    }

    return &json;
}

template<typename T>
bool ParseLegacyValue(const nlohmann::json& json, T& out_value) {
    return GetJsonValue(
        __SHORT_FILE__, __FUNCTION__, __LINE__, json, out_value, false, "", false);
}

bool ParseLegacyValue(const nlohmann::json& json, WPDynamicValue::Type hint, WPDynamicValue& out_value) {
    switch (hint) {
        case WPDynamicValue::Type::Boolean: {
            bool value = false;
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(value);
            return true;
        }
        case WPDynamicValue::Type::Int32: {
            int32_t value = 0;
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(value);
            return true;
        }
        case WPDynamicValue::Type::UInt32: {
            uint32_t value = 0;
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(value);
            return true;
        }
        case WPDynamicValue::Type::Float: {
            float value = 0.0f;
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(value);
            return true;
        }
        case WPDynamicValue::Type::Double: {
            double value = 0.0;
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(value);
            return true;
        }
        case WPDynamicValue::Type::String: {
            std::string value;
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(std::move(value));
            return true;
        }
        case WPDynamicValue::Type::FloatVector: {
            std::vector<float> value;
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(std::move(value));
            return true;
        }
        case WPDynamicValue::Type::Int3: {
            std::array<int32_t, 3> value {};
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(value);
            return true;
        }
        case WPDynamicValue::Type::Float2: {
            std::array<float, 2> value {};
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(value);
            return true;
        }
        case WPDynamicValue::Type::Float3: {
            std::array<float, 3> value {};
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(value);
            return true;
        }
        case WPDynamicValue::Type::Float4: {
            std::array<float, 4> value {};
            if (!ParseLegacyValue(json, value)) return false;
            out_value = WPDynamicValue(value);
            return true;
        }
        case WPDynamicValue::Type::Null:
            return false;
    }

    return false;
}

WPDynamicValue MakeInactiveConditionValue(WPDynamicValue::Type type) {
    // Conditional user bindings represent authored selector branches. If the selector does not
    // match, the branch should contribute a neutral value instead of falling back to the branch's
    // authored payload; otherwise a `value=true` branch such as camera parallax would stay enabled
    // even when the combo selects the opposite option.
    switch (type) {
        case WPDynamicValue::Type::Boolean:
            return WPDynamicValue(false);
        case WPDynamicValue::Type::Int32:
            return WPDynamicValue(int32_t { 0 });
        case WPDynamicValue::Type::UInt32:
            return WPDynamicValue(uint32_t { 0 });
        case WPDynamicValue::Type::Float:
            return WPDynamicValue(0.0f);
        case WPDynamicValue::Type::Double:
            return WPDynamicValue(0.0);
        case WPDynamicValue::Type::String:
            return WPDynamicValue(std::string {});
        case WPDynamicValue::Type::FloatVector:
            return WPDynamicValue(std::vector<float> {});
        case WPDynamicValue::Type::Int3:
            return WPDynamicValue(std::array<int32_t, 3> {});
        case WPDynamicValue::Type::Float2:
            return WPDynamicValue(std::array<float, 2> {});
        case WPDynamicValue::Type::Float3:
            return WPDynamicValue(std::array<float, 3> {});
        case WPDynamicValue::Type::Float4:
            return WPDynamicValue(std::array<float, 4> {});
        case WPDynamicValue::Type::Null:
            return WPDynamicValue {};
    }

    return WPDynamicValue {};
}

} // namespace

WPDynamicValue WPUserSetting::evaluate(const UserPropertyMap*           user_properties,
                                       WPScriptRuntime*                 runtime,
                                       const WPScriptEvaluationContext& base_context) const {
    (void)runtime;
    (void)base_context;

    WPDynamicValue resolved = value;

    if (property.has_value()) {
        if (const auto* user_value = LookupUserProperty(user_properties, property->name)) {
            const auto* user_entry = FindUserPropertyEntry(user_properties, property->name);
            if (!property->condition.empty()) {
                // Object-form `user` bindings with a condition are selector branches: the user
                // property decides whether this authored branch is active, while `setting.value`
                // remains the value to apply. This keeps combo option "0" from being converted
                // directly into bool false for properties whose branch value is actually true.
                if (user_entry != nullptr &&
                    MatchesUserPropertyCondition(*user_entry, property->condition)) {
                    resolved = value;
                } else {
                    resolved = MakeInactiveConditionValue(value.type());
                }
            } else {
                if (const auto override_value =
                        WPDynamicValue::FromUserPropertyValue(*user_value, value.type());
                    override_value.has_value()) {
                    resolved = *override_value;
                }
            }
        }
    }

    return resolved;
}

bool ParseUserSetting(const nlohmann::json& json, WPUserSetting& setting, WPDynamicValue::Type hint) {
    setting = {};

    const auto* value_node = ResolveInitialValueNode(json);
    if (value_node == nullptr) return false;

    std::optional<WPDynamicValue> parsed_value;
    if (hint == WPDynamicValue::Type::Null) {
        parsed_value = WPDynamicValue::FromJsonLiteral(*value_node, hint);
    } else {
        WPDynamicValue legacy_value;
        if (ParseLegacyValue(*value_node, hint, legacy_value)) {
            parsed_value = legacy_value;
        } else {
            parsed_value = WPDynamicValue::FromJsonLiteral(*value_node, hint);
        }
    }
    if (! parsed_value.has_value()) return false;

    setting.value = *parsed_value;

    if (json.is_object()) {
        setting.property = ParseUserBinding(json);

        if (json.contains("script") && json.at("script").is_string()) {
            setting.script = json.at("script").get<std::string>();
        }

        if (json.contains("scriptproperties") && json.at("scriptproperties").is_object()) {
            for (const auto& [name, property_json] : json.at("scriptproperties").items()) {
                auto nested = std::make_shared<WPUserSetting>();
                if (ParseUserSetting(property_json, *nested)) {
                    setting.script_properties.emplace(name, std::move(nested));
                }
            }
        }
    }

    return true;
}

bool ParseUserSetting(const nlohmann::json& json, WPUserSetting& setting) {
    return ParseUserSetting(json, setting, WPDynamicValue::Type::Null);
}

} // namespace wallpaper
