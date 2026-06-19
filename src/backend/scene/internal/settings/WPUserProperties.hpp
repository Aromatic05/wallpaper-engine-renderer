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
#include <vector>

#include "scene/SceneShader.h"

namespace wallpaper
{

struct UserPropertyBinding {
    std::string name;
    std::string condition;

    bool empty() const noexcept { return name.empty(); }
};

struct VisibleBinding {
    bool                value { true };
    UserPropertyBinding user;

    bool hasUserBinding() const noexcept { return ! user.empty(); }
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

inline bool MatchesUserPropertyCondition(const UserProperty& property,
                                         std::string_view   condition) {
    const auto trimmed = TrimString(condition);
    if (trimmed.empty()) return IsUserPropertyTruthy(property.value);

    char* endptr = nullptr;
    const auto expected = std::strtof(trimmed.c_str(), &endptr);
    if (endptr != nullptr && *endptr == '\0') {
        // Wallpaper Engine checkboxes behave like a 2-state selector where
        // condition "0" means enabled and condition "1" means disabled.
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

inline const UserProperty* FindUserPropertyEntry(const UserPropertyMap* properties,
                                                 std::string_view       name) {
    if (! properties) return nullptr;

    const auto it = properties->find(std::string(name));
    return it == properties->end() ? nullptr : &it->second;
}

inline UserPropertyValue MakeUserPropertyBool(bool value) {
    return ShaderValue(value ? 1.0f : 0.0f);
}

inline UserPropertyValue MakeUserPropertyNumber(float value) {
    return ShaderValue(value);
}

inline bool TryParseExpressionNumber(const UserPropertyValue& value, float& result) {
    if (const auto* shader_value = std::get_if<ShaderValue>(&value)) {
        if (shader_value->size() == 0) return false;
        result = (*shader_value)[0];
        return true;
    }

    if (const auto* string_value = std::get_if<std::string>(&value)) {
        const auto trimmed = TrimString(*string_value);
        if (trimmed.empty()) return false;

        char* endptr = nullptr;
        result       = std::strtof(trimmed.c_str(), &endptr);
        return endptr != nullptr && *endptr == '\0';
    }

    return false;
}

inline bool AreUserPropertyValuesEqual(const UserPropertyValue& lhs, const UserPropertyValue& rhs) {
    float lhs_number = 0.0f;
    float rhs_number = 0.0f;
    if (TryParseExpressionNumber(lhs, lhs_number) && TryParseExpressionNumber(rhs, rhs_number)) {
        return std::abs(lhs_number - rhs_number) < 0.0001f;
    }

    if (const auto* lhs_string = std::get_if<std::string>(&lhs)) {
        if (const auto* rhs_string = std::get_if<std::string>(&rhs)) {
            return TrimString(*lhs_string) == TrimString(*rhs_string);
        }
    }

    return IsUserPropertyTruthy(lhs) == IsUserPropertyTruthy(rhs);
}

inline bool CompareUserPropertyValues(const UserPropertyValue& lhs, const UserPropertyValue& rhs,
                                      std::string_view op) {
    if (op == "==") return AreUserPropertyValuesEqual(lhs, rhs);
    if (op == "!=") return !AreUserPropertyValuesEqual(lhs, rhs);

    float lhs_number = 0.0f;
    float rhs_number = 0.0f;
    if (!TryParseExpressionNumber(lhs, lhs_number) || !TryParseExpressionNumber(rhs, rhs_number)) {
        return false;
    }

    if (op == "<") return lhs_number < rhs_number;
    if (op == "<=") return lhs_number <= rhs_number;
    if (op == ">") return lhs_number > rhs_number;
    if (op == ">=") return lhs_number >= rhs_number;
    return false;
}

inline bool EvaluateUserPropertyExpression(std::string_view                expression,
                                           const UserPropertyMap*          properties,
                                           std::vector<std::string>*       stack = nullptr);

inline bool IsUserPropertyEnabled(std::string_view          name,
                                  const UserPropertyMap*    properties,
                                  std::vector<std::string>* stack = nullptr) {
    const auto* entry = FindUserPropertyEntry(properties, name);
    if (entry == nullptr) return false;
    if (entry->condition.empty()) return true;

    std::vector<std::string> owned_stack;
    if (stack == nullptr) stack = &owned_stack;

    const auto it = std::find(stack->begin(), stack->end(), std::string(name));
    if (it != stack->end()) return false;

    stack->push_back(std::string(name));
    const bool enabled = EvaluateUserPropertyExpression(entry->condition, properties, stack);
    stack->pop_back();
    return enabled;
}

inline const UserPropertyValue* LookupUserProperty(std::string_view          name,
                                                   const UserPropertyMap*    properties,
                                                   std::vector<std::string>* stack = nullptr) {
    const auto* entry = FindUserPropertyEntry(properties, name);
    if (entry == nullptr) return nullptr;
    if (!IsUserPropertyEnabled(name, properties, stack)) return nullptr;
    return &entry->value;
}

inline const UserPropertyValue* LookupUserProperty(const UserPropertyMap* properties,
                                                   std::string_view       name) {
    return LookupUserProperty(name, properties, nullptr);
}

inline const ShaderValue* LookupUserPropertyShaderValue(const UserPropertyMap* properties,
                                                        std::string_view       name) {
    const auto* property = LookupUserProperty(properties, name);
    return property != nullptr ? std::get_if<ShaderValue>(property) : nullptr;
}

inline const std::string* LookupUserPropertyString(const UserPropertyMap* properties,
                                                   std::string_view       name) {
    const auto* property = LookupUserProperty(properties, name);
    return property != nullptr ? std::get_if<std::string>(property) : nullptr;
}

class UserPropertyExpressionParser {
public:
    UserPropertyExpressionParser(std::string_view          expression,
                                 const UserPropertyMap*    properties,
                                 std::vector<std::string>* stack)
        : m_expression(expression), m_properties(properties), m_stack(stack) {}

    bool Evaluate() {
        auto value = ParseOr();
        SkipSpace();
        return value.has_value() && m_pos == m_expression.size() && IsUserPropertyTruthy(*value);
    }

private:
    std::optional<UserPropertyValue> ParseOr() {
        auto lhs = ParseAnd();
        while (lhs.has_value()) {
            SkipSpace();
            if (!Consume("||")) break;
            auto rhs = ParseAnd();
            if (!rhs.has_value()) return std::nullopt;
            lhs = MakeUserPropertyBool(IsUserPropertyTruthy(*lhs) || IsUserPropertyTruthy(*rhs));
        }
        return lhs;
    }

    std::optional<UserPropertyValue> ParseAnd() {
        auto lhs = ParseEquality();
        while (lhs.has_value()) {
            SkipSpace();
            if (!Consume("&&")) break;
            auto rhs = ParseEquality();
            if (!rhs.has_value()) return std::nullopt;
            lhs = MakeUserPropertyBool(IsUserPropertyTruthy(*lhs) && IsUserPropertyTruthy(*rhs));
        }
        return lhs;
    }

    std::optional<UserPropertyValue> ParseEquality() {
        auto lhs = ParseUnary();
        while (lhs.has_value()) {
            SkipSpace();

            std::string_view op;
            if (Consume("==")) {
                op = "==";
            } else if (Consume("!=")) {
                op = "!=";
            } else if (Consume("<=")) {
                op = "<=";
            } else if (Consume(">=")) {
                op = ">=";
            } else if (Consume("<")) {
                op = "<";
            } else if (Consume(">")) {
                op = ">";
            } else {
                break;
            }

            auto rhs = ParseUnary();
            if (!rhs.has_value()) return std::nullopt;

            lhs = MakeUserPropertyBool(CompareUserPropertyValues(*lhs, *rhs, op));
        }
        return lhs;
    }

    std::optional<UserPropertyValue> ParseUnary() {
        SkipSpace();
        if (Consume("!")) {
            auto value = ParseUnary();
            if (!value.has_value()) return std::nullopt;
            return MakeUserPropertyBool(!IsUserPropertyTruthy(*value));
        }
        return ParsePrimary();
    }

    std::optional<UserPropertyValue> ParsePrimary() {
        SkipSpace();
        if (Consume("(")) {
            auto value = ParseOr();
            SkipSpace();
            if (!Consume(")")) return std::nullopt;
            return value;
        }

        if (auto string_value = ParseStringLiteral(); string_value.has_value()) {
            return UserPropertyValue(*string_value);
        }

        if (auto number_value = ParseNumberLiteral(); number_value.has_value()) {
            return MakeUserPropertyNumber(*number_value);
        }

        if (auto identifier = ParseIdentifier(); identifier.has_value()) {
            if (*identifier == "true") return MakeUserPropertyBool(true);
            if (*identifier == "false") return MakeUserPropertyBool(false);

            SkipSpace();
            if (Consume(".value")) {
                return LookupIdentifier(*identifier);
            }
            return LookupIdentifier(*identifier);
        }

        return std::nullopt;
    }

    std::optional<UserPropertyValue> LookupIdentifier(std::string_view identifier) const {
        if (const auto* property = LookupUserProperty(identifier, m_properties, m_stack)) {
            return *property;
        }
        return std::nullopt;
    }

    std::optional<std::string> ParseStringLiteral() {
        SkipSpace();
        if (m_pos >= m_expression.size()) return std::nullopt;

        const char quote = m_expression[m_pos];
        if (quote != '\'' && quote != '"') return std::nullopt;

        m_pos++;
        std::string out;
        while (m_pos < m_expression.size()) {
            const char ch = m_expression[m_pos++];
            if (ch == quote) return out;
            if (ch == '\\' && m_pos < m_expression.size()) {
                out.push_back(m_expression[m_pos++]);
            } else {
                out.push_back(ch);
            }
        }
        return std::nullopt;
    }

    std::optional<float> ParseNumberLiteral() {
        SkipSpace();
        const size_t start = m_pos;
        if (m_pos < m_expression.size() &&
            (m_expression[m_pos] == '+' || m_expression[m_pos] == '-')) {
            m_pos++;
        }

        bool seen_digit = false;
        bool seen_dot   = false;
        while (m_pos < m_expression.size()) {
            const char ch = m_expression[m_pos];
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                seen_digit = true;
                m_pos++;
                continue;
            }
            if (ch == '.' && !seen_dot) {
                seen_dot = true;
                m_pos++;
                continue;
            }
            break;
        }

        if (!seen_digit) {
            m_pos = start;
            return std::nullopt;
        }

        char* endptr = nullptr;
        const auto number =
            std::strtof(std::string(m_expression.substr(start, m_pos - start)).c_str(), &endptr);
        if (endptr == nullptr || *endptr != '\0') {
            m_pos = start;
            return std::nullopt;
        }
        return number;
    }

    std::optional<std::string> ParseIdentifier() {
        SkipSpace();
        if (m_pos >= m_expression.size()) return std::nullopt;

        const auto is_ident_start = [](char ch) {
            return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_';
        };
        const auto is_ident_continue = [](char ch) {
            return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
        };

        if (!is_ident_start(m_expression[m_pos])) return std::nullopt;

        const size_t start = m_pos++;
        while (m_pos < m_expression.size() && is_ident_continue(m_expression[m_pos])) {
            m_pos++;
        }
        return std::string(m_expression.substr(start, m_pos - start));
    }

    void SkipSpace() {
        while (m_pos < m_expression.size() &&
               std::isspace(static_cast<unsigned char>(m_expression[m_pos]))) {
            m_pos++;
        }
    }

    bool Consume(std::string_view token) {
        if (m_expression.substr(m_pos, token.size()) != token) return false;
        m_pos += token.size();
        return true;
    }

    std::string_view          m_expression;
    const UserPropertyMap*    m_properties { nullptr };
    std::vector<std::string>* m_stack { nullptr };
    size_t                    m_pos { 0 };
};

inline bool EvaluateUserPropertyExpression(std::string_view                expression,
                                           const UserPropertyMap*          properties,
                                           std::vector<std::string>*       stack) {
    const auto trimmed = TrimString(expression);
    if (trimmed.empty()) return true;

    UserPropertyExpressionParser parser(trimmed, properties, stack);
    return parser.Evaluate();
}

inline bool EvaluateVisibleBinding(const VisibleBinding& binding,
                                   const UserPropertyMap* properties) {
    if (! binding.hasUserBinding()) return binding.value;

    const auto* property = FindUserPropertyEntry(properties, binding.user.name);
    if (! property || !IsUserPropertyEnabled(binding.user.name, properties, nullptr)) return binding.value;

    return MatchesUserPropertyCondition(*property, binding.user.condition);
}

} // namespace wallpaper
