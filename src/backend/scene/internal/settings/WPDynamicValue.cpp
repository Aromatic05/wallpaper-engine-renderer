#include "WPDynamicValue.hpp"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "utils/String.h"

namespace wallpaper
{
namespace
{

std::string ShaderValueToString(const ShaderValue& value) {
    std::ostringstream out;
    for (size_t i = 0; i < value.size(); i++) {
        if (i != 0) out << ' ';
        out << value[i];
    }
    return out.str();
}

// These helpers intentionally keep every debug payload self-describing so text-layer
// logs can print dynamic property values without needing the caller to know the
// concrete storage type that travelled through bindings, scripts, or animations.
std::string EscapeDebugString(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            case '\'':
                escaped += "\\'";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

template<typename T, size_t N>
void AppendArrayToStream(std::ostringstream& out, const std::array<T, N>& value) {
    out << '[';
    for (size_t i = 0; i < N; i++) {
        if (i != 0) out << ", ";
        if constexpr (std::is_floating_point_v<T>) {
            out << std::fixed << std::setprecision(6) << value[i];
        } else {
            out << value[i];
        }
    }
    out << ']';
}

template<typename T>
void AppendVectorToStream(std::ostringstream& out, const std::vector<T>& value) {
    out << '[';
    for (size_t i = 0; i < value.size(); i++) {
        if (i != 0) out << ", ";
        if constexpr (std::is_floating_point_v<T>) {
            out << std::fixed << std::setprecision(6) << value[i];
        } else {
            out << value[i];
        }
    }
    out << ']';
}

const char* DynamicValueTypeName(WPDynamicValue::Type type) {
    switch (type) {
        case WPDynamicValue::Type::Null:
            return "null";
        case WPDynamicValue::Type::Boolean:
            return "bool";
        case WPDynamicValue::Type::Int32:
            return "int32";
        case WPDynamicValue::Type::UInt32:
            return "uint32";
        case WPDynamicValue::Type::Float:
            return "float";
        case WPDynamicValue::Type::Double:
            return "double";
        case WPDynamicValue::Type::String:
            return "string";
        case WPDynamicValue::Type::FloatVector:
            return "floatVector";
        case WPDynamicValue::Type::Int3:
            return "int3";
        case WPDynamicValue::Type::Float2:
            return "float2";
        case WPDynamicValue::Type::Float3:
            return "float3";
        case WPDynamicValue::Type::Float4:
            return "float4";
    }
    return "unknown";
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
    } catch (const utils::StrToArray::WrongSizeExp&) {
        return false;
    } catch (const std::invalid_argument&) {
        return false;
    } catch (const std::out_of_range&) {
        return false;
    }
}

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

std::optional<WPDynamicValue> ConvertPropertyVector(const ShaderValue&           value,
                                                    WPDynamicValue::Type         hint) {
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
        case WPDynamicValue::Type::Float2: {
            if (value.size() == 1) {
                return WPDynamicValue(std::array<float, 2> { value[0], value[0] });
            }
            if (value.size() < 2) return std::nullopt;
            return WPDynamicValue(std::array<float, 2> { value[0], value[1] });
        }
        case WPDynamicValue::Type::Float3: {
            if (value.size() == 1) {
                return WPDynamicValue(std::array<float, 3> { value[0], value[0], value[0] });
            }
            if (value.size() < 3) return std::nullopt;
            return WPDynamicValue(std::array<float, 3> { value[0], value[1], value[2] });
        }
        case WPDynamicValue::Type::Float4: {
            if (value.size() == 1) {
                return WPDynamicValue(
                    std::array<float, 4> { value[0], value[0], value[0], value[0] });
            }
            if (value.size() < 4) return std::nullopt;
            return WPDynamicValue(std::array<float, 4> { value[0], value[1], value[2], value[3] });
        }
        case WPDynamicValue::Type::FloatVector: {
            std::vector<float> out_values(value.size());
            for (size_t i = 0; i < value.size(); i++) {
                out_values[i] = value[i];
            }
            return WPDynamicValue(std::move(out_values));
        }
        case WPDynamicValue::Type::String: {
            std::string out;
            for (size_t i = 0; i < value.size(); i++) {
                if (i != 0) out.push_back(' ');
                out += std::to_string(value[i]);
            }
            return WPDynamicValue(std::move(out));
        }
        case WPDynamicValue::Type::Int3: {
            if (value.size() == 1) {
                return WPDynamicValue(
                    std::array<int32_t, 3> { static_cast<int32_t>(value[0]),
                                             static_cast<int32_t>(value[0]),
                                             static_cast<int32_t>(value[0]) });
            }
            if (value.size() < 3) return std::nullopt;
            return WPDynamicValue(
                std::array<int32_t, 3> { static_cast<int32_t>(value[0]),
                                         static_cast<int32_t>(value[1]),
                                         static_cast<int32_t>(value[2]) });
        }
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
    out << "type=" << DynamicValueTypeName(type()) << ", value=";

    if (const auto* value = std::get_if<bool>(&m_storage)) {
        out << (*value ? "true" : "false");
        return out.str();
    }
    if (const auto* value = std::get_if<int32_t>(&m_storage)) {
        out << *value;
        return out.str();
    }
    if (const auto* value = std::get_if<uint32_t>(&m_storage)) {
        out << *value;
        return out.str();
    }
    if (const auto* value = std::get_if<float>(&m_storage)) {
        out << std::fixed << std::setprecision(6) << *value;
        return out.str();
    }
    if (const auto* value = std::get_if<double>(&m_storage)) {
        out << std::fixed << std::setprecision(6) << *value;
        return out.str();
    }
    if (const auto* value = std::get_if<std::string>(&m_storage)) {
        out << "'" << EscapeDebugString(*value) << "'"
            << " (length=" << value->size() << ")";
        return out.str();
    }
    if (const auto* value = std::get_if<std::vector<float>>(&m_storage)) {
        AppendVectorToStream(out, *value);
        out << " (length=" << value->size() << ")";
        return out.str();
    }
    if (const auto* value = std::get_if<std::array<int32_t, 3>>(&m_storage)) {
        AppendArrayToStream(out, *value);
        return out.str();
    }
    if (const auto* value = std::get_if<std::array<float, 2>>(&m_storage)) {
        AppendArrayToStream(out, *value);
        return out.str();
    }
    if (const auto* value = std::get_if<std::array<float, 3>>(&m_storage)) {
        AppendArrayToStream(out, *value);
        return out.str();
    }
    if (const auto* value = std::get_if<std::array<float, 4>>(&m_storage)) {
        AppendArrayToStream(out, *value);
        return out.str();
    }

    out << "null";
    return out.str();
}

std::optional<WPScriptValue> WPDynamicValue::toScriptValue() const {
    if (const auto* value = std::get_if<bool>(&m_storage)) {
        return WPScriptValue::Boolean(*value);
    }
    if (const auto* value = std::get_if<int32_t>(&m_storage)) {
        return WPScriptValue::Number(*value);
    }
    if (const auto* value = std::get_if<uint32_t>(&m_storage)) {
        return WPScriptValue::Number(*value);
    }
    if (const auto* value = std::get_if<float>(&m_storage)) {
        return WPScriptValue::Number(*value);
    }
    if (const auto* value = std::get_if<double>(&m_storage)) {
        return WPScriptValue::Number(*value);
    }
    if (const auto* value = std::get_if<std::string>(&m_storage)) {
        return WPScriptValue::String(*value);
    }
    if (const auto* value = std::get_if<std::vector<float>>(&m_storage)) {
        return WPScriptValue::NumberArray(std::vector<double>(value->begin(), value->end()));
    }
    if (const auto* value = std::get_if<std::array<int32_t, 3>>(&m_storage)) {
        return WPScriptValue::NumberArray(
            { static_cast<double>((*value)[0]),
              static_cast<double>((*value)[1]),
              static_cast<double>((*value)[2]) });
    }
    if (const auto* value = std::get_if<std::array<float, 2>>(&m_storage)) {
        return WPScriptValue::NumberArray({ (*value)[0], (*value)[1] });
    }
    if (const auto* value = std::get_if<std::array<float, 3>>(&m_storage)) {
        return WPScriptValue::NumberArray({ (*value)[0], (*value)[1], (*value)[2] });
    }
    if (const auto* value = std::get_if<std::array<float, 4>>(&m_storage)) {
        return WPScriptValue::NumberArray({ (*value)[0], (*value)[1], (*value)[2], (*value)[3] });
    }
    return std::nullopt;
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
            return ParseScalar(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                            : std::nullopt;
        }
        case Type::FloatVector: {
            std::vector<float> value;
            return ParseJsonArray(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                               : std::nullopt;
        }
        case Type::Int3: {
            std::array<int32_t, 3> value {};
            return ParseJsonArray(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                               : std::nullopt;
        }
        case Type::Float2: {
            std::array<float, 2> value {};
            return ParseJsonArray(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                               : std::nullopt;
        }
        case Type::Float3: {
            std::array<float, 3> value {};
            return ParseJsonArray(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                               : std::nullopt;
        }
        case Type::Float4: {
            std::array<float, 4> value {};
            return ParseJsonArray(json, value) ? std::optional<WPDynamicValue>(WPDynamicValue(value))
                                               : std::nullopt;
        }
        case Type::Null:
            break;
    }

    if (json.is_boolean()) return WPDynamicValue(json.get<bool>());
    if (json.is_number_unsigned()) return WPDynamicValue(json.get<uint32_t>());
    if (json.is_number_integer()) return WPDynamicValue(json.get<int32_t>());
    if (json.is_number_float()) return WPDynamicValue(json.get<float>());

    if (json.is_array()) {
        std::vector<float> values;
        if (! ParseJsonArray(json, values)) return std::nullopt;
        if (values.size() == 2) return WPDynamicValue(std::array<float, 2> { values[0], values[1] });
        if (values.size() == 3) return WPDynamicValue(std::array<float, 3> { values[0], values[1], values[2] });
        if (values.size() == 4) return WPDynamicValue(std::array<float, 4> { values[0], values[1], values[2], values[3] });
        return WPDynamicValue(std::move(values));
    }

    if (json.is_string()) {
        const auto& text = json.get_ref<const std::string&>();
        std::array<float, 4> values {};
        if (ParseJsonArray(json, values)) return WPDynamicValue(values);
        std::array<float, 3> values3 {};
        if (ParseJsonArray(json, values3)) return WPDynamicValue(values3);
        std::array<float, 2> values2 {};
        if (ParseJsonArray(json, values2)) return WPDynamicValue(values2);
        return WPDynamicValue(text);
    }

    return std::nullopt;
}

std::optional<WPDynamicValue> WPDynamicValue::FromJsonLiteral(const nlohmann::json& json) {
    return FromJsonLiteral(json, Type::Null);
}

std::optional<WPDynamicValue> WPDynamicValue::FromUserPropertyValue(const UserPropertyValue& property,
                                                                    Type                     hint) {
    if (const auto* vector_value = std::get_if<ShaderValue>(&property)) {
        return ConvertPropertyVector(*vector_value, hint);
    }

    const auto& string_value = std::get<std::string>(property);
    switch (hint) {
        case Type::Boolean:
            return WPDynamicValue(IsUserPropertyTruthy(property));
        case Type::String:
            return WPDynamicValue(string_value);
        case Type::Int32:
        case Type::UInt32:
        case Type::Float:
        case Type::Double:
        case Type::FloatVector:
        case Type::Int3:
        case Type::Float2:
        case Type::Float3:
        case Type::Float4: {
            if (auto parsed = FromJsonLiteral(nlohmann::json(string_value), hint); parsed.has_value()) {
                return parsed;
            }
            return std::nullopt;
        }
        case Type::Null:
            break;
    }

    if (auto parsed = FromJsonLiteral(nlohmann::json(string_value), Type::Null); parsed.has_value()) {
        return parsed;
    }
    return WPDynamicValue(string_value);
}

std::optional<WPDynamicValue> WPDynamicValue::FromScriptValue(const WPScriptValue& value, Type hint) {
    switch (hint) {
        case Type::Boolean:
            return WPDynamicValue(value.boolean_value);
        case Type::Int32:
            return WPDynamicValue(value.numeric_values.empty() ? 0
                                                               : static_cast<int32_t>(value.numeric_values.front()));
        case Type::UInt32:
            return WPDynamicValue(value.numeric_values.empty() ? 0u
                                                               : static_cast<uint32_t>(value.numeric_values.front()));
        case Type::Float:
            return WPDynamicValue(value.numeric_values.empty() ? 0.0f
                                                               : static_cast<float>(value.numeric_values.front()));
        case Type::Double:
            return WPDynamicValue(value.numeric_values.empty() ? 0.0
                                                               : value.numeric_values.front());
        case Type::String:
            return WPDynamicValue(value.string_value);
        case Type::Float2:
            if (value.numeric_values.size() == 1) {
                return WPDynamicValue(std::array<float, 2> {
                    static_cast<float>(value.numeric_values.front()),
                    static_cast<float>(value.numeric_values.front()),
                });
            }
            if (value.numeric_values.size() < 2) return std::nullopt;
            return WPDynamicValue(std::array<float, 2> {
                static_cast<float>(value.numeric_values[0]),
                static_cast<float>(value.numeric_values[1]),
            });
        case Type::Float3:
            if (value.numeric_values.size() == 1) {
                return WPDynamicValue(std::array<float, 3> {
                    static_cast<float>(value.numeric_values.front()),
                    static_cast<float>(value.numeric_values.front()),
                    static_cast<float>(value.numeric_values.front()),
                });
            }
            if (value.numeric_values.size() < 3) return std::nullopt;
            return WPDynamicValue(std::array<float, 3> {
                static_cast<float>(value.numeric_values[0]),
                static_cast<float>(value.numeric_values[1]),
                static_cast<float>(value.numeric_values[2]),
            });
        case Type::Float4:
            if (value.numeric_values.size() == 1) {
                return WPDynamicValue(std::array<float, 4> {
                    static_cast<float>(value.numeric_values.front()),
                    static_cast<float>(value.numeric_values.front()),
                    static_cast<float>(value.numeric_values.front()),
                    static_cast<float>(value.numeric_values.front()),
                });
            }
            if (value.numeric_values.size() < 4) return std::nullopt;
            return WPDynamicValue(std::array<float, 4> {
                static_cast<float>(value.numeric_values[0]),
                static_cast<float>(value.numeric_values[1]),
                static_cast<float>(value.numeric_values[2]),
                static_cast<float>(value.numeric_values[3]),
            });
        case Type::FloatVector: {
            std::vector<float> values(value.numeric_values.size());
            for (size_t i = 0; i < value.numeric_values.size(); i++) {
                values[i] = static_cast<float>(value.numeric_values[i]);
            }
            return WPDynamicValue(std::move(values));
        }
        case Type::Int3:
            if (value.numeric_values.size() == 1) {
                return WPDynamicValue(std::array<int32_t, 3> {
                    static_cast<int32_t>(value.numeric_values.front()),
                    static_cast<int32_t>(value.numeric_values.front()),
                    static_cast<int32_t>(value.numeric_values.front()),
                });
            }
            if (value.numeric_values.size() < 3) return std::nullopt;
            return WPDynamicValue(std::array<int32_t, 3> {
                static_cast<int32_t>(value.numeric_values[0]),
                static_cast<int32_t>(value.numeric_values[1]),
                static_cast<int32_t>(value.numeric_values[2]),
            });
        case Type::Null:
            break;
    }

    if (value.shape == WPScriptValueShape::String) return WPDynamicValue(value.string_value);
    if (value.shape == WPScriptValueShape::Boolean) return WPDynamicValue(value.boolean_value);
    if (value.numeric_values.size() == 1) return WPDynamicValue(static_cast<float>(value.numeric_values.front()));
    return FromJsonLiteral(nlohmann::json(value.numeric_values));
}

} // namespace wallpaper
