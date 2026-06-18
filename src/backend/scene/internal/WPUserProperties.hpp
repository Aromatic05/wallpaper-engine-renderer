#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

#include "scene/SceneShader.h"

namespace wallpaper
{

struct UserPropertyBinding {
    std::string name;
    std::string condition;

    bool empty() const noexcept { return name.empty(); }
};

using UserPropertyValue = std::variant<ShaderValue, std::string>;

struct UserProperty {
    UserPropertyValue value;
    std::string       condition;
    bool              is_boolean { false };
};

using UserPropertyMap = std::unordered_map<std::string, UserProperty>;

inline std::string TrimString(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};

    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

inline std::string LowerString(std::string_view value) {
    std::string out = TrimString(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

inline bool IsUserPropertyTruthy(const UserPropertyValue& value) {
    if (const auto* shader_value = std::get_if<ShaderValue>(&value)) {
        for (size_t i = 0; i < shader_value->size(); i++) {
            if (std::abs((*shader_value)[i]) > 0.0001f) return true;
        }
        return false;
    }

    const auto lowered = LowerString(std::get<std::string>(value));
    return ! lowered.empty() && lowered != "0" && lowered != "false";
}

inline const UserProperty* FindUserPropertyEntry(const UserPropertyMap* properties,
                                                 std::string_view       name) {
    if (properties == nullptr) return nullptr;

    const auto it = properties->find(std::string(name));
    return it == properties->end() ? nullptr : &it->second;
}

inline bool MatchesUserPropertyCondition(const UserProperty& property,
                                         std::string_view   condition) {
    const auto trimmed = TrimString(condition);
    if (trimmed.empty()) return IsUserPropertyTruthy(property.value);

    char* endptr = nullptr;
    const auto expected = std::strtof(trimmed.c_str(), &endptr);
    if (endptr != nullptr && *endptr == '\0') {
        if (property.is_boolean) {
            if (std::abs(expected) < 0.0001f) return IsUserPropertyTruthy(property.value);
            if (std::abs(expected - 1.0f) < 0.0001f) return ! IsUserPropertyTruthy(property.value);
            return false;
        }

        if (const auto* shader_value = std::get_if<ShaderValue>(&property.value)) {
            return shader_value->size() > 0 && std::abs((*shader_value)[0] - expected) < 0.0001f;
        }

        const auto* string_value = std::get_if<std::string>(&property.value);
        return string_value != nullptr && TrimString(*string_value) == trimmed;
    }

    if (const auto lowered = LowerString(trimmed); lowered == "true" || lowered == "false") {
        return IsUserPropertyTruthy(property.value) == (lowered == "true");
    }

    if (const auto* string_value = std::get_if<std::string>(&property.value)) {
        return TrimString(*string_value) == trimmed;
    }

    return false;
}

inline const UserPropertyValue* LookupUserProperty(const UserPropertyMap* properties,
                                                   std::string_view       name) {
    const auto* entry = FindUserPropertyEntry(properties, name);
    return entry == nullptr ? nullptr : &entry->value;
}

} // namespace wallpaper
