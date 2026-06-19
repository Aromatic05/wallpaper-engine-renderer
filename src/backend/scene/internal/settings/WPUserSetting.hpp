#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

#include "WPDynamicValue.hpp"

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
    bool isDynamic() const noexcept { return hasUserBinding(); }

    WPDynamicValue evaluate(const UserPropertyMap*             user_properties,
                            WPScriptRuntime*                   runtime,
                            const WPScriptEvaluationContext&   base_context) const;

    template<typename T>
    bool evaluateAs(T* out_value,
                    const UserPropertyMap*           user_properties,
                    WPScriptRuntime*                 runtime,
                    const WPScriptEvaluationContext& base_context) const {
        return evaluate(user_properties, runtime, base_context).tryGet(out_value);
    }

    template<typename T>
    T evaluateOr(const T& fallback,
                 const UserPropertyMap*           user_properties,
                 WPScriptRuntime*                 runtime,
                 const WPScriptEvaluationContext& base_context) const {
        T value = fallback;
        evaluateAs(&value, user_properties, runtime, base_context);
        return value;
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
