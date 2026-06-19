#include "WPDynamicValue.hpp"

#include <cmath>
#include <cstdlib>
#include <sstream>

#include <nlohmann/json.hpp>

#include "utils/String.h"

namespace wallpaper
{
namespace
{

template<typename T>
bool ParseScalar(const nlohmann::json& json, T& out_value) {
    if constexpr (std::is_same_v<T, bool>) {
        if (json.is_boolean()) {
            out_value = json.get<bool>();
            return true;
        }

        if (json.is_number()) {
            out_value = std::abs(json.get<double>()) >= 0.0001;
            return true;
        }

        if (! json.is_string()) return false;

        const auto lowered = LowerString(json.get_ref<const std::string&>());
        if (lowered == "true") {
            out_value = true;
            return true;
        }
        if (lowered == "false") {
            out_value = false;
            return true;
        }

        char* endptr = nullptr;
        const auto value = std::strtod(lowered.c_str(), &endptr);
        if (endptr == nullptr || *endptr != '\0') return false;
        out_value = std::abs(value) >= 0.0001;
        return true;
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (! json.is_string()) return false;
        out_value = json.get<std::string>();
        return true;
    } else {
        if (json.is_number()) {
            out_value = json.get<T>();
            return true;
        }

        if (! json.is_string()) return false;
        const auto text = TrimString(json.get_ref<const std::string&>());
        if constexpr (std::is_integral_v<T>) {
            char* endptr = nullptr;
            const auto value = std::strtol(text.c_str(), &endptr, 10);
            if (endptr == nullptr || *endptr != '\0') return false;
            out_value = static_cast<T>(value);
            return true;
        } else {
            char* endptr = nullptr;
            const auto value = std::strtod(text.c_str(), &endptr);
            if (endptr == nullptr || *endptr != '\0') return false;
            out_value = static_cast<T>(value);
            return true;
        }
    }
}

template<typename T>
bool ParseJsonArray(const nlohmann::json& json, std::vector<T>& out_values) {
    if (! json.is_array()) return false;

    out_values.clear();
    out_values.reserve(json.size());
    for (const auto& item : json) {
        if (! item.is_number()) return false;
        out_values.push_back(item.get<T>());
    }
    return true;
}

template<typename T, size_t N>
bool ParseJsonArray(const nlohmann::json& json, std::array<T, N>& out_values) {
    if (json.is_array()) {
        if (json.size() != N) return false;
        for (size_t i = 0; i < N; i++) {
            if (! json.at(i).is_number()) return false;
            out_values[i] = json.at(i).get<T>();
        }
        return true;
    }

    if (! json.is_string()) return false;
    try {
        return utils::StrToArray::Convert(json.get<std::string>(), out_values);
    } catch (...) {
        return false;
    }
}

std::optional<WPDynamicValue> ConvertPropertyVector(const ShaderValue& value,
                                                    WPDynamicValue::Type hint) {
    switch (hint) {
    case WPDynamicValue::Type::Boolean:
        return WPDynamicValue(value.size() > 0 && std::abs(value[0]) > 0.0001f);
    case WPDynamicValue::Type::Int32:
        return WPDynamicValue(value.size() == 0 ? 0 : static_cast<int32_t>(value[0]));
    case WPDynamicValue::Type::UInt32:
        return WPDynamicValue(value.size() == 0 ? 0u : static_cast<uint32_t>(value[0]));
    case WPDynamicValue::Type::Double:
        return WPDynamicValue(value.size() == 0 ? 0.0 : static_cast<double>(value[0]));
    case WPDynamicValue::Type::Float:
    case WPDynamicValue::Type::Null:
        return WPDynamicValue(value.size() == 0 ? 0.0f : value[0]);
    case WPDynamicValue::Type::Float2:
        if (value.size() < 2) return std::nullopt;
        return WPDynamicValue(std::array<float, 2> { value[0], value[1] });
    case WPDynamicValue::Type::Float3:
        if (value.size() < 3) return std::nullopt;
        return WPDynamicValue(std::array<float, 3> { value[0], value[1], value[2] });
    case WPDynamicValue::Type::Float4:
        if (value.size() < 4) return std::nullopt;
        return WPDynamicValue(std::array<float, 4> { value[0], value[1], value[2], value[3] });
    case WPDynamicValue::Type::FloatVector: {
        std::vector<float> out_values(value.size());
        for (size_t i = 0; i < value.size(); i++) {
            out_values[i] = value[i];
        }
        return WPDynamicValue(std::move(out_values));
    }
    case WPDynamicValue::Type::String: {
        std::ostringstream out;
        for (size_t i = 0; i < value.size(); i++) {
            if (i != 0) out << ' ';
            out << value[i];
        }
        return WPDynamicValue(out.str());
    }
    case WPDynamicValue::Type::Int3:
        if (value.size() < 3) return std::nullopt;
        return WPDynamicValue(std::array<int32_t, 3> { static_cast<int32_t>(value[0]),
                                                       static_cast<int32_t>(value[1]),
                                                       static_cast<int32_t>(value[2]) });
    }

    return std::nullopt;
}

} // namespace

WPDynamicValue::Type WPDynamicValue::type() const noexcept {
    return static_cast<Type>(m_storage.index());
}

bool WPDynamicValue::isNull() const noexcept {
    return std::holds_alternative<std::monostate>(m_storage);
}

bool WPDynamicValue::equals(const WPDynamicValue& other) const noexcept {
    return m_storage == other.m_storage;
}

std::string WPDynamicValue::describe() const {
    std::ostringstream out;
    out << "type=" << static_cast<int>(type());
    return out.str();
}

std::optional<WPDynamicValue> WPDynamicValue::FromJsonLiteral(const nlohmann::json& json, Type hint) {
    switch (hint) {
    case Type::Boolean: {
        bool value = false;
        return ParseScalar(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                        : std::nullopt;
    }
    case Type::Int32: {
        int32_t value = 0;
        return ParseScalar(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                        : std::nullopt;
    }
    case Type::UInt32: {
        uint32_t value = 0;
        return ParseScalar(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                        : std::nullopt;
    }
    case Type::Float: {
        float value = 0.0f;
        return ParseScalar(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                        : std::nullopt;
    }
    case Type::Double: {
        double value = 0.0;
        return ParseScalar(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                        : std::nullopt;
    }
    case Type::String: {
        std::string value;
        return ParseScalar(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(std::move(value)))
                                        : std::nullopt;
    }
    case Type::FloatVector: {
        std::vector<float> values;
        return ParseJsonArray(json, values) ? std::optional<WPDynamicValue>(WPDynamicValue(std::move(values)))
                                            : std::nullopt;
    }
    case Type::Int3: {
        std::array<int32_t, 3> values {};
        return ParseJsonArray(json, values) ? std::optional<WPDynamicValue>(WPDynamicValue(values))
                                            : std::nullopt;
    }
    case Type::Float2: {
        std::array<float, 2> values {};
        return ParseJsonArray(json, values) ? std::optional<WPDynamicValue>(WPDynamicValue(values))
                                            : std::nullopt;
    }
    case Type::Float3: {
        std::array<float, 3> values {};
        return ParseJsonArray(json, values) ? std::optional<WPDynamicValue>(WPDynamicValue(values))
                                            : std::nullopt;
    }
    case Type::Float4: {
        std::array<float, 4> values {};
        return ParseJsonArray(json, values) ? std::optional<WPDynamicValue>(WPDynamicValue(values))
                                            : std::nullopt;
    }
    case Type::Null:
        break;
    }

    if (json.is_null()) return WPDynamicValue {};
    if (json.is_boolean()) return WPDynamicValue(json.get<bool>());
    if (json.is_number_unsigned()) return WPDynamicValue(json.get<uint32_t>());
    if (json.is_number_integer()) return WPDynamicValue(json.get<int32_t>());
    if (json.is_number_float()) return WPDynamicValue(json.get<float>());
    if (json.is_string()) return WPDynamicValue(json.get<std::string>());

    std::vector<float> values;
    if (ParseJsonArray(json, values)) {
        if (values.size() == 2) return WPDynamicValue(std::array<float, 2> { values[0], values[1] });
        if (values.size() == 3) return WPDynamicValue(std::array<float, 3> { values[0], values[1], values[2] });
        if (values.size() == 4) return WPDynamicValue(std::array<float, 4> { values[0], values[1], values[2], values[3] });
        return WPDynamicValue(std::move(values));
    }

    return std::nullopt;
}

std::optional<WPDynamicValue> WPDynamicValue::FromJsonLiteral(const nlohmann::json& json) {
    return FromJsonLiteral(json, Type::Null);
}

std::optional<WPDynamicValue> WPDynamicValue::FromUserPropertyValue(const UserPropertyValue& property,
                                                                    Type hint) {
    if (const auto* shader_value = std::get_if<ShaderValue>(&property)) {
        return ConvertPropertyVector(*shader_value, hint);
    }

    const auto& string_value = std::get<std::string>(property);
    return FromJsonLiteral(nlohmann::json(string_value), hint);
}

} // namespace wallpaper
