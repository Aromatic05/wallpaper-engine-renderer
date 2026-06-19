#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

#include "settings/WPDynamicValue.hpp"

namespace wallpaper
{

struct WPUserSetting {
    WPDynamicValue value {};
    std::optional<UserPropertyBinding> property;
    std::string script;
    std::unordered_map<std::string, std::shared_ptr<WPUserSetting>> script_properties;

    bool hasUserBinding() const noexcept {
        return property.has_value() && ! property->empty();
    }

    bool hasScript() const noexcept { return ! script.empty(); }
    bool isDynamic() const noexcept { return hasUserBinding() || hasScript(); }

    WPDynamicValue evaluate(const UserPropertyMap* user_properties) const;

    template<typename T>
    bool evaluateAs(T* out_value, const UserPropertyMap* user_properties) const {
        return evaluate(user_properties).tryGet(out_value);
    }

    template<typename T>
    T evaluateOr(const T& fallback, const UserPropertyMap* user_properties) const {
        T value_out = fallback;
        evaluateAs(&value_out, user_properties);
        return value_out;
    }
};

bool ParseUserSetting(const nlohmann::json& json, WPUserSetting& setting, WPDynamicValue::Type hint);
bool ParseUserSetting(const nlohmann::json& json, WPUserSetting& setting);

template<typename T>
bool ParseUserSetting(const nlohmann::json& json, WPUserSetting& setting, const T& fallback) {
    if (! ParseUserSetting(json, setting, WPDynamicValue::TypeFor<T>())) return false;

    T parsed_value = fallback;
    if (setting.value.tryGet(&parsed_value)) {
        setting.value = WPDynamicValue(parsed_value);
    } else {
        setting.value = WPDynamicValue(fallback);
    }
    return true;
}

} // namespace wallpaper
