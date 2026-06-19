#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "WPScriptRuntime.hpp"
#include "WPUserProperties.hpp"

namespace wallpaper
{

class WPDynamicValue {
public:
    enum class Type
    {
        Null,
        Boolean,
        Int32,
        UInt32,
        Float,
        Double,
        String,
        FloatVector,
        Int3,
        Float2,
        Float3,
        Float4,
    };

    using Storage = std::variant<std::monostate,
                                 bool,
                                 int32_t,
                                 uint32_t,
                                 float,
                                 double,
                                 std::string,
                                 std::vector<float>,
                                 std::array<int32_t, 3>,
                                 std::array<float, 2>,
                                 std::array<float, 3>,
                                 std::array<float, 4>>;

    WPDynamicValue() = default;

    template<typename T>
    explicit WPDynamicValue(T value): m_storage(std::move(value)) {}

    Type type() const noexcept;
    bool isNull() const noexcept;
    bool equals(const WPDynamicValue& other) const noexcept;
    std::string describe() const;

    template<typename T>
    bool tryGet(T* out_value) const;

    std::optional<WPScriptValue> toScriptValue() const;

    static std::optional<WPDynamicValue> FromJsonLiteral(const nlohmann::json& json, Type hint);
    static std::optional<WPDynamicValue> FromJsonLiteral(const nlohmann::json& json);
    static std::optional<WPDynamicValue> FromUserPropertyValue(const UserPropertyValue& property,
                                                               Type                     hint);
    static std::optional<WPDynamicValue> FromScriptValue(const WPScriptValue& value, Type hint);

    template<typename T>
    static constexpr Type TypeFor();

private:
    Storage m_storage {};
};

template<typename T>
constexpr WPDynamicValue::Type WPDynamicValue::TypeFor() {
    if constexpr (std::is_same_v<T, bool>) {
        return Type::Boolean;
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return Type::Int32;
    } else if constexpr (std::is_same_v<T, uint32_t>) {
        return Type::UInt32;
    } else if constexpr (std::is_same_v<T, float>) {
        return Type::Float;
    } else if constexpr (std::is_same_v<T, double>) {
        return Type::Double;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return Type::String;
    } else if constexpr (std::is_same_v<T, std::vector<float>>) {
        return Type::FloatVector;
    } else if constexpr (std::is_same_v<T, std::array<int32_t, 3>>) {
        return Type::Int3;
    } else if constexpr (std::is_same_v<T, std::array<float, 2>>) {
        return Type::Float2;
    } else if constexpr (std::is_same_v<T, std::array<float, 3>>) {
        return Type::Float3;
    } else if constexpr (std::is_same_v<T, std::array<float, 4>>) {
        return Type::Float4;
    } else {
        return Type::Null;
    }
}

template<typename T>
bool WPDynamicValue::tryGet(T* out_value) const {
    if (out_value == nullptr) return false;

    if (const auto* exact = std::get_if<T>(&m_storage)) {
        *out_value = *exact;
        return true;
    }

    if constexpr (std::is_same_v<T, float>) {
        if (const auto* value = std::get_if<double>(&m_storage)) {
            *out_value = static_cast<float>(*value);
            return true;
        }
        if (const auto* value = std::get_if<int32_t>(&m_storage)) {
            *out_value = static_cast<float>(*value);
            return true;
        }
        if (const auto* value = std::get_if<uint32_t>(&m_storage)) {
            *out_value = static_cast<float>(*value);
            return true;
        }
    }

    if constexpr (std::is_same_v<T, double>) {
        if (const auto* value = std::get_if<float>(&m_storage)) {
            *out_value = static_cast<double>(*value);
            return true;
        }
        if (const auto* value = std::get_if<int32_t>(&m_storage)) {
            *out_value = static_cast<double>(*value);
            return true;
        }
        if (const auto* value = std::get_if<uint32_t>(&m_storage)) {
            *out_value = static_cast<double>(*value);
            return true;
        }
    }

    if constexpr (std::is_same_v<T, int32_t>) {
        if (const auto* value = std::get_if<float>(&m_storage)) {
            *out_value = static_cast<int32_t>(*value);
            return true;
        }
        if (const auto* value = std::get_if<double>(&m_storage)) {
            *out_value = static_cast<int32_t>(*value);
            return true;
        }
        if (const auto* value = std::get_if<uint32_t>(&m_storage)) {
            *out_value = static_cast<int32_t>(*value);
            return true;
        }
    }

    if constexpr (std::is_same_v<T, uint32_t>) {
        if (const auto* value = std::get_if<float>(&m_storage)) {
            *out_value = static_cast<uint32_t>(*value);
            return true;
        }
        if (const auto* value = std::get_if<double>(&m_storage)) {
            *out_value = static_cast<uint32_t>(*value);
            return true;
        }
        if (const auto* value = std::get_if<int32_t>(&m_storage)) {
            *out_value = static_cast<uint32_t>(*value);
            return true;
        }
    }

    return false;
}

} // namespace wallpaper
