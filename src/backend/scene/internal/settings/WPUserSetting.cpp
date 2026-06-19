#include "WPUserSetting.hpp"

#include <nlohmann/json.hpp>

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
        if (value.is_object() && value.contains("value")) return ResolveInitialValueNode(value);
        return &value;
    }
    return &json;
}

WPDynamicValue MakeInactiveConditionValue(WPDynamicValue::Type type) {
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

WPDynamicValue WPUserSetting::evaluate(const UserPropertyMap* user_properties) const {
    WPDynamicValue resolved = value;

    if (! property.has_value()) return resolved;

    const auto* user_entry = FindUserPropertyEntry(user_properties, property->name);
    if (user_entry == nullptr) return resolved;

    if (! property->condition.empty()) {
        if (MatchesUserPropertyCondition(*user_entry, property->condition)) {
            return value;
        }
        return MakeInactiveConditionValue(value.type());
    }

    if (const auto override_value = WPDynamicValue::FromUserPropertyValue(user_entry->value, value.type());
        override_value.has_value()) {
        resolved = *override_value;
    }

    return resolved;
}

bool ParseUserSetting(const nlohmann::json& json, WPUserSetting& setting, WPDynamicValue::Type hint) {
    setting = {};

    const auto* value_node = ResolveInitialValueNode(json);
    if (value_node == nullptr) return false;

    auto parsed_value = WPDynamicValue::FromJsonLiteral(*value_node, hint);
    if (! parsed_value.has_value()) return false;
    setting.value = *parsed_value;

    if (! json.is_object()) return true;

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

    return true;
}

bool ParseUserSetting(const nlohmann::json& json, WPUserSetting& setting) {
    return ParseUserSetting(json, setting, WPDynamicValue::Type::Null);
}

} // namespace wallpaper
