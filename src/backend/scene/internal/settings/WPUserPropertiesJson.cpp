#include "WPUserPropertiesJson.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wallpaper
{
Result<UserPropertyMap> ParseUserPropertiesJson(std::string_view jsonText) {
    auto root = nlohmann::json::parse(jsonText.begin(), jsonText.end(), nullptr, false, true);
    if (root.is_discarded()) {
        return Result<UserPropertyMap>::failure(ResultCode::InvalidArgument,
                                                "user properties are not valid JSON");
    }
    if (! root.is_object()) {
        return Result<UserPropertyMap>::failure(ResultCode::InvalidArgument,
                                                "user properties must be a JSON object");
    }

    UserPropertyMap result;
    result.reserve(root.size());
    for (auto it = root.begin(); it != root.end(); ++it) {
        UserProperty property;
        const auto&  value = it.value();
        if (value.is_boolean()) {
            property.value      = ShaderValue(value.get<bool>() ? 1.0f : 0.0f);
            property.is_boolean = true;
        } else if (value.is_number()) {
            property.value = ShaderValue(static_cast<float>(value.get<double>()));
        } else if (value.is_string()) {
            property.value = value.get<std::string>();
        } else if (value.is_array() && ! value.empty()) {
            std::vector<float> components;
            components.reserve(value.size());
            for (const auto& component : value) {
                if (! component.is_number()) {
                    return Result<UserPropertyMap>::failure(
                        ResultCode::InvalidArgument,
                        "user property '" + it.key() + "' array must contain only numbers");
                }
                components.push_back(static_cast<float>(component.get<double>()));
            }
            property.value = ShaderValue(components);
        } else {
            return Result<UserPropertyMap>::failure(
                ResultCode::InvalidArgument,
                "user property '" + it.key()
                    + "' must be a boolean, number, string, or non-empty numeric array");
        }
        result.emplace(it.key(), std::move(property));
    }

    return Result<UserPropertyMap>::success(std::move(result));
}

UserPropertyMap MergeUserPropertiesWithDefaults(const UserPropertyMap& defaults,
                                                const UserPropertyMap& overrides) {
    UserPropertyMap merged = defaults;
    for (const auto& [name, overrideProperty] : overrides) {
        auto existing = merged.find(name);
        if (existing == merged.end()) {
            merged.emplace(name, overrideProperty);
            continue;
        }

        UserProperty combined = overrideProperty;
        if (combined.condition.empty()) combined.condition = existing->second.condition;
        if (existing->second.is_boolean) combined.is_boolean = true;
        existing->second = std::move(combined);
    }
    return merged;
}
} // namespace wallpaper
