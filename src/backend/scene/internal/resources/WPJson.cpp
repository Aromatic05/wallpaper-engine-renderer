#include "WPJson.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_map>

#include "utils/Identity.hpp"
#include "utils/String.h"
#include "WPScriptRuntime.hpp"

namespace wallpaper
{
namespace
{
thread_local const UserPropertyMap* g_json_user_properties = nullptr;
thread_local const nlohmann::json*  g_json_scene_root      = nullptr;

template<typename T>
struct IsStdVector : std::false_type {};

template<typename T, typename Allocator>
struct IsStdVector<std::vector<T, Allocator>> : std::true_type {};

struct StaticCanvasSize {
    double x { 0.0 };
    double y { 0.0 };
    bool   valid { false };
};

template<typename T>
bool TryParseNumber(std::string_view text, T& value);

std::string ShortenForLog(std::string_view text, size_t max_length);
std::string ShaderValueToString(const ShaderValue& value);
std::string FormatNumericVectorString(const std::vector<double>& values);

std::string DescribeUserPropertyValue(const UserPropertyValue& value) {
    if (const auto* shader_value = std::get_if<ShaderValue>(&value)) {
        return std::string("shader(") + ShaderValueToString(*shader_value) + ")";
    }
    return std::string("string(\"") + ShortenForLog(std::get<std::string>(value), 160) + "\")";
}

template<typename T>
std::string DescribeConvertedOverrideValue(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

template<typename T>
std::string DescribeConvertedOverrideValue(const std::vector<T>& value) {
    std::ostringstream out;
    out << "[";
    for (size_t index = 0; index < value.size(); index++) {
        if (index != 0) out << ", ";
        out << value[index];
    }
    out << "]";
    return out.str();
}

template<typename T, size_t N>
std::string DescribeConvertedOverrideValue(const std::array<T, N>& value) {
    std::ostringstream out;
    out << "[";
    for (size_t index = 0; index < N; index++) {
        if (index != 0) out << ", ";
        out << value[index];
    }
    out << "]";
    return out.str();
}

std::string DescribeScriptValue(const WPScriptValue& value) {
    switch (value.shape) {
        case WPScriptValueShape::Boolean:
            return value.boolean_value ? "bool(true)" : "bool(false)";
        case WPScriptValueShape::String:
            return std::string("string(\"") + value.string_value + "\")";
        case WPScriptValueShape::NumberArray:
            return std::string("array(") + FormatNumericVectorString(value.numeric_values) + ")";
        case WPScriptValueShape::VectorString:
            return std::string("vector(\"") + FormatNumericVectorString(value.numeric_values) + "\")";
        case WPScriptValueShape::Number:
        default: {
            std::ostringstream out;
            out << "number(" << (value.numeric_values.empty() ? 0.0 : value.numeric_values.front())
                << ")";
            return out.str();
        }
    }
}

std::string ShortenForLog(std::string_view text, size_t max_length = 160) {
    if (text.size() <= max_length) return std::string(text);
    return std::string(text.substr(0, max_length)) + "...";
}

std::optional<UserPropertyBinding> ResolveUserPropertyBinding(const nlohmann::json& json) {
    if (! json.is_object() || ! json.contains("user") || json.at("user").is_null()) {
        return std::nullopt;
    }

    UserPropertyBinding binding;
    const auto&         user = json.at("user");
    if (user.is_string()) {
        GET_JSON_VALUE_NOWARN(user, binding.name);
    } else if (user.is_object()) {
        GET_JSON_NAME_VALUE_NOWARN(user, "name", binding.name);
        GET_JSON_NAME_VALUE_NOWARN(user, "condition", binding.condition);
    }

    if (binding.name.empty()) return std::nullopt;
    return binding;
}

const nlohmann::json* ResolveAnimatedInitialValue(const nlohmann::json& json) {
    if (! json.is_object() || ! json.contains("animation")) return nullptr;

    const auto& animation = json.at("animation");
    if (! animation.is_object()) return nullptr;

    bool start_paused { false };
    if (animation.contains("options") && animation.at("options").is_object()) {
        GET_JSON_NAME_VALUE_NOWARN(animation.at("options"), "startpaused", start_paused);
    }
    if (! start_paused || ! animation.contains("c0")) return nullptr;

    const auto& c0 = animation.at("c0");
    if (! c0.is_array() || c0.empty() || ! c0.front().is_object() ||
        ! c0.front().contains("value")) {
        return nullptr;
    }

    return &c0.front().at("value");
}

const nlohmann::json& ResolvePropertyValueNode(const nlohmann::json& json) {
    if (const auto* animated = ResolveAnimatedInitialValue(json)) return *animated;
    if (json.is_object() && json.contains("value")) return json.at("value");
    return json;
}

std::optional<nlohmann::json> TryResolveUserPropertyOverrideJson(const nlohmann::json& json) {
    if (g_json_user_properties == nullptr) return std::nullopt;

    const auto binding = ResolveUserPropertyBinding(json);
    if (! binding.has_value()) return std::nullopt;

    const auto* property = LookupUserProperty(g_json_user_properties, binding->name);
    const auto* property_entry = FindUserPropertyEntry(g_json_user_properties, binding->name);
    if (property == nullptr || property_entry == nullptr) return std::nullopt;

    if (! binding->condition.empty() &&
        ! MatchesUserPropertyCondition(*property_entry, binding->condition)) {
        return std::nullopt;
    }

    if (const auto* shader_value = std::get_if<ShaderValue>(property)) {
        if (shader_value->size() == 1) return (*shader_value)[0];
        return ShaderValueToString(*shader_value);
    }

    return std::get<std::string>(*property);
}

bool TryReadJsonNumber(const nlohmann::json& json, double& value) {
    try {
        if (json.is_number()) {
            value = json.get<double>();
            return true;
        }
        if (json.is_boolean()) {
            value = json.get<bool>() ? 1.0 : 0.0;
            return true;
        }
        if (json.is_string()) {
            const auto text = TrimString(json.get<std::string>());
            if (text.empty()) return false;

            char* endptr = nullptr;
            value        = std::strtod(text.c_str(), &endptr);
            return endptr != nullptr && *endptr == '\0';
        }
    } catch (const nlohmann::json::exception&) {
    }
    return false;
}

std::optional<StaticCanvasSize> TryReadCanvasSize(const nlohmann::json& root) {
    if (! root.is_object() || ! root.contains("general") || ! root.at("general").is_object()) {
        return std::nullopt;
    }

    const auto& general = root.at("general");
    if (! general.contains("orthogonalprojection") ||
        ! general.at("orthogonalprojection").is_object()) {
        return std::nullopt;
    }

    const auto& ortho = general.at("orthogonalprojection");
    double      width = 0.0;
    double      height = 0.0;
    if (! ortho.contains("width") || ! ortho.contains("height") ||
        ! TryReadJsonNumber(ortho.at("width"), width) ||
        ! TryReadJsonNumber(ortho.at("height"), height)) {
        return std::nullopt;
    }

    return StaticCanvasSize { width, height, true };
}

bool TryParseNumberVectorString(const std::string& source, std::vector<double>& out) {
    std::vector<float> values;
    if (! utils::StrToArray::Convert(source, values)) return false;

    out.resize(values.size());
    for (size_t i = 0; i < values.size(); i++) {
        out[i] = values[i];
    }
    return true;
}

std::string FormatNumericVectorString(const std::vector<double>& values) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(5);
    for (size_t i = 0; i < values.size(); i++) {
        if (i != 0) out << ' ';
        out << values[i];
    }
    return out.str();
}

std::optional<WPScriptValue> TryParseScriptValueJson(const nlohmann::json& value_node) {
    if (value_node.is_number()) {
        return WPScriptValue::Number(value_node.get<double>());
    }

    if (value_node.is_boolean()) {
        return WPScriptValue::Boolean(value_node.get<bool>());
    }

    if (value_node.is_array()) {
        std::vector<double> values;
        values.reserve(value_node.size());
        for (const auto& item : value_node) {
            double component = 0.0;
            if (! TryReadJsonNumber(item, component)) return std::nullopt;
            values.push_back(component);
        }
        if (values.empty()) return std::nullopt;
        return WPScriptValue::NumberArray(std::move(values));
    }

    if (value_node.is_string()) {
        const auto text = value_node.get<std::string>();

        std::vector<double> values;
        if (TryParseNumberVectorString(text, values) && ! values.empty()) {
            return WPScriptValue::VectorString(std::move(values));
        }

        return WPScriptValue::String(text);
    }

    return std::nullopt;
}

std::optional<WPScriptValue> TryReadScriptValueState(const nlohmann::json& node) {
    return TryParseScriptValueJson(ResolvePropertyValueNode(node));
}

std::optional<WPScriptValue> TryResolveScriptPropertyValue(const nlohmann::json& json) {
    if (const auto overridden = TryResolveUserPropertyOverrideJson(json); overridden.has_value()) {
        const auto parsed = TryParseScriptValueJson(*overridden);
        if (!parsed.has_value()) {
            LOG_ERROR("SceneScript: failed to parse scriptproperty override json: %s",
                      overridden->dump().c_str());
        }
        return parsed;
    }

    const auto parsed = TryReadScriptValueState(json);
    if (!parsed.has_value()) {
        LOG_ERROR("SceneScript: failed to parse scriptproperty default json: %s",
                  json.dump().c_str());
    }
    return parsed;
}

nlohmann::json SerializeScriptValue(const WPScriptValue& value) {
    switch (value.shape) {
        case WPScriptValueShape::Boolean:
            return value.boolean_value;
        case WPScriptValueShape::String:
            return value.string_value;
        case WPScriptValueShape::NumberArray:
            return nlohmann::json(value.numeric_values);
        case WPScriptValueShape::VectorString:
            return FormatNumericVectorString(value.numeric_values);
        case WPScriptValueShape::Number:
        default:
            return value.numeric_values.empty() ? 0.0 : value.numeric_values.front();
    }
}

WPScriptRuntime& GetScriptRuntime() {
    thread_local WPScriptRuntime runtime;
    return runtime;
}

std::optional<nlohmann::json> TryResolveScriptValueNode(const nlohmann::json& node,
                                                        std::string_view      property_name = {}) {
    if (! node.is_object() || ! node.contains("script") || ! node.at("script").is_string()) {
        return std::nullopt;
    }
    if (ResolveUserPropertyBinding(node).has_value()) return std::nullopt;

    const auto current_value = TryReadScriptValueState(node);
    if (! current_value.has_value()) {
        LOG_ERROR("SceneScript: failed to parse current value for script node: %s",
                  node.dump().c_str());
        return std::nullopt;
    }

    WPScriptEvaluationContext context;
    // Parser-time script evaluation runs before the persistent host exists, so pass the authored
    // property name through to the lightweight runtime. That lets the wrapper seed exactly
    // thisLayer.origin, thisLayer.scale, etc. with the current base value instead of leaving every
    // layer property at a generic zero fallback.
    context.property_name = std::string(property_name);
    if (g_json_scene_root != nullptr) {
        if (const auto canvas_size = TryReadCanvasSize(*g_json_scene_root);
            canvas_size.has_value() && canvas_size->valid) {
            context.canvas_size = { canvas_size->x, canvas_size->y };
        }
    }

    if (node.contains("scriptproperties") && ! node.at("scriptproperties").is_null()) {
        if (! node.at("scriptproperties").is_object()) return std::nullopt;
        for (const auto& [name, property_node] : node.at("scriptproperties").items()) {
            const auto property_value = TryResolveScriptPropertyValue(property_node);
            if (! property_value.has_value()) {
                LOG_ERROR("SceneScript: failed to resolve scriptproperty '%s'", name.c_str());
                return std::nullopt;
            }
            context.script_properties.emplace(name, *property_value);
        }
    }

    auto& runtime = GetScriptRuntime();
    if (! runtime.isReady()) {
        LOG_ERROR("SceneScript: QuickJS runtime is not ready");
        return std::nullopt;
    }

    const auto evaluated =
        runtime.evaluate(node.at("script").get_ref<const std::string&>(), *current_value, context);
    if (! evaluated.has_value()) {
        // Parser-time script execution is only a best-effort value probe. The persistent
        // QuickJS host will run the real Wallpaper Engine callbacks after the scene is loaded,
        // so falling back to the authored raw value is expected recovery rather than a fatal
        // scene loading error.
        LOG_INFO("SceneScript: runtime evaluation failed, falling back to raw value");
        return std::nullopt;
    }

    return SerializeScriptValue(*evaluated);
}

template<>
bool TryParseNumber<float>(std::string_view text, float& value) {
    char* endptr = nullptr;
    value        = std::strtof(std::string(text).c_str(), &endptr);
    return endptr != nullptr && *endptr == '\0';
}

template<>
bool TryParseNumber<double>(std::string_view text, double& value) {
    char* endptr = nullptr;
    value        = std::strtod(std::string(text).c_str(), &endptr);
    return endptr != nullptr && *endptr == '\0';
}

template<>
bool TryParseNumber<int32_t>(std::string_view text, int32_t& value) {
    char* endptr = nullptr;
    value        = (int32_t)std::strtol(std::string(text).c_str(), &endptr, 10);
    return endptr != nullptr && *endptr == '\0';
}

template<>
bool TryParseNumber<uint32_t>(std::string_view text, uint32_t& value) {
    char* endptr = nullptr;
    value        = (uint32_t)std::strtoul(std::string(text).c_str(), &endptr, 10);
    return endptr != nullptr && *endptr == '\0';
}

std::string ShaderValueToString(const ShaderValue& value) {
    std::ostringstream out;
    for (size_t i = 0; i < value.size(); i++) {
        if (i != 0) out << ' ';
        out << value[i];
    }
    return out.str();
}

template<typename T>
bool TryConvertUserPropertyValue(const UserPropertyValue& property, T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        value = IsUserPropertyTruthy(property);
        return true;
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (const auto* string_value = std::get_if<std::string>(&property)) {
            value = *string_value;
            return true;
        }
        if (const auto* shader_value = std::get_if<ShaderValue>(&property)) {
            value = ShaderValueToString(*shader_value);
            return true;
        }
        return false;
    } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double> ||
                         std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>) {
        if (const auto* shader_value = std::get_if<ShaderValue>(&property)) {
            if (shader_value->size() == 0) return false;
            value = (T)(*shader_value)[0];
            return true;
        }
        if (const auto* string_value = std::get_if<std::string>(&property)) {
            return TryParseNumber<T>(TrimString(*string_value), value);
        }
        return false;
    } else {
        return false;
    }
}

template<typename T>
bool TryConvertUserPropertyValue(const UserPropertyValue& property, std::vector<T>& value) {
    if (const auto* shader_value = std::get_if<ShaderValue>(&property)) {
        if (shader_value->size() == 1 && !value.empty()) {
            std::fill(value.begin(), value.end(), (T)(*shader_value)[0]);
            return true;
        }
        value.resize(shader_value->size());
        for (size_t i = 0; i < shader_value->size(); i++) {
            value[i] = (T)(*shader_value)[i];
        }
        return true;
    }
    if (const auto* string_value = std::get_if<std::string>(&property)) {
        return utils::StrToArray::Convert(*string_value, value);
    }
    return false;
}

template<typename T, std::size_t N>
bool TryConvertUserPropertyValue(const UserPropertyValue& property, std::array<T, N>& value) {
    if (const auto* shader_value = std::get_if<ShaderValue>(&property)) {
        if (shader_value->size() == 1) {
            value.fill((T)(*shader_value)[0]);
            return true;
        }
        if (shader_value->size() != N) return false;
        for (size_t i = 0; i < N; i++) {
            value[i] = (T)(*shader_value)[i];
        }
        return true;
    }
    if (const auto* string_value = std::get_if<std::string>(&property)) {
        return utils::StrToArray::Convert(*string_value, value);
    }
    return false;
}

template<typename T>
bool TryGetUserPropertyOverride(const nlohmann::json& json, T& value) {
    if (g_json_user_properties == nullptr) return false;

    const auto binding = ResolveUserPropertyBinding(json);
    if (! binding.has_value()) return false;

    const auto* property = LookupUserProperty(g_json_user_properties, binding->name);
    const auto* property_entry = FindUserPropertyEntry(g_json_user_properties, binding->name);
    if (property == nullptr || property_entry == nullptr) return false;

    if (! binding->condition.empty()) {
        // Wallpaper Engine uses object-form `user` bindings with `condition` as a branch gate, not
        // as a direct value source. For example, a combo value "0" can mean "Open" while the actual
        // property value is the authored JSON `value=true`; converting that raw "0" string to bool
        // would incorrectly disable camera parallax. When the branch matches, fall through to the
        // authored value node. When it does not match, consume the property with an inactive zero
        // value so the authored branch value cannot leak into the wrong selector state.
        if (MatchesUserPropertyCondition(*property_entry, binding->condition)) {
            return false;
        }
        value = T {};
        return true;
    }

    const bool converted = TryConvertUserPropertyValue(*property, value);
    if (!converted) {
        LOG_ERROR("SceneScript: failed to convert direct user binding '%s' raw=%s condition='%s'",
                  binding->name.c_str(),
                  DescribeUserPropertyValue(property_entry->value).c_str(),
                  binding->condition.c_str());
    }
    return converted;
}
} // namespace

bool ParseJson(const char* file, const char* func, int line, const std::string& source,
               nlohmann::json& result) {
    try {
        result = nlohmann::json::parse(source);
    } catch (nlohmann::json::parse_error& e) {
        WallpaperLog(LOGLEVEL_ERROR, file, line, "parse json(%s), %s", func, e.what());
        return false;
    }
    return true;
}

template<typename T>
inline bool _GetJsonValue(const nlohmann::json&                  json,
                          typename utils::is_std_array<T>::type& value,
                          std::string_view                       property_name = {}) {
    if (TryGetUserPropertyOverride(json, value)) return true;

    const auto scripted = TryResolveScriptValueNode(json, property_name);
    const auto& njson   = scripted.has_value() ? *scripted : ResolvePropertyValueNode(json);

    using Tv = typename T::value_type;
    if (njson.is_number()) {
        value = { njson.get<Tv>() };
        return true;
    }

    if (njson.is_array()) {
        if constexpr (IsStdVector<T>::value) {
            value.clear();
            value.reserve(njson.size());
            for (const auto& item : njson) {
                double component = 0.0;
                if (! TryReadJsonNumber(item, component)) return false;
                value.push_back((Tv)component);
            }
            return true;
        } else {
            if (njson.size() != std::tuple_size_v<T>) return false;

            size_t index = 0;
            for (const auto& item : njson) {
                double component = 0.0;
                if (! TryReadJsonNumber(item, component)) return false;
                value[index++] = (Tv)component;
            }
            return true;
        }
    }

    std::string strvalue = njson.get<std::string>();
    return utils::StrToArray::Convert(strvalue, value);
}

template<typename T>
inline bool _GetJsonValue(const nlohmann::json& json, T& value,
                          std::string_view property_name = {}) {
    if (TryGetUserPropertyOverride(json, value)) return true;

    if (const auto scripted = TryResolveScriptValueNode(json, property_name);
        scripted.has_value()) {
        value = scripted->get<T>();
        return true;
    }

    value = ResolvePropertyValueNode(json).get<T>();
    return true;
}

template<typename T>
inline bool _GetJsonValue(const char* file, const char* func, int line, const nlohmann::json& json,
                          T& value, bool warn, const char* name) {
    (void)warn;

    using njson = nlohmann::json;
    std::string nameinfo;
    if (name != nullptr) nameinfo = std::string("(key: ") + name + ")";
    try {
        const std::string_view property_name =
            name != nullptr ? std::string_view(name) : std::string_view {};
        return _GetJsonValue<T>(json, value, property_name);
    } catch (const njson::type_error& e) {
        WallpaperLog(LOGLEVEL_INFO,
                     file,
                     line,
                     "%s %s at %s\n%s",
                     e.what(),
                     nameinfo.c_str(),
                     func,
                     json.dump(4).c_str());
    } catch (const std::invalid_argument& e) {
        WallpaperLog(LOGLEVEL_ERROR, file, line, "%s %s at %s", e.what(), nameinfo.c_str(), func);
    } catch (const std::out_of_range& e) {
        WallpaperLog(LOGLEVEL_ERROR, file, line, "%s %s at %s", e.what(), nameinfo.c_str(), func);
    } catch (const utils::StrToArray::WrongSizeExp& e) {
        WallpaperLog(LOGLEVEL_ERROR, file, line, "%s %s at %s", e.what(), nameinfo.c_str(), func);
    }
    return false;
}

template<typename T>
typename JsonTemplateTypeCheck<T>::type
GetJsonValue(const char* file, const char* func, int line, const nlohmann::json& json, T& value,
             bool has_name, std::string_view name_view, bool warn) {
    std::string name { name_view };
    if (has_name) {
        if (! json.contains(name)) {
            if (warn && WallpaperVerboseLogEnabled())
                WallpaperLog(LOGLEVEL_INFO,
                             "",
                             0,
                             "read json \"%s\" not a key at %s(%s:%d)",
                             name.data(),
                             func,
                             file,
                             line);
            return false;
        } else if (json.at(name).is_null()) {
            if (warn && WallpaperVerboseLogEnabled())
                WallpaperLog(LOGLEVEL_INFO,
                             "",
                             0,
                             "read json \"%s\" is null at %s(%s:%d)",
                             name.data(),
                             func,
                             file,
                             line);
            return false;
        }
    }
    return _GetJsonValue<T>(file,
                            func,
                            line,
                            has_name ? json.at(name) : json,
                            value,
                            warn,
                            name.empty() ? nullptr : name.c_str());
}

#define T_IMPL_GET_JSON(TYPE)                                                            \
    template JsonTemplateTypeCheck<TYPE>::type GetJsonValue<TYPE>(const char*,           \
                                                                  const char*,           \
                                                                  int,                   \
                                                                  const nlohmann::json&, \
                                                                  TYPE&,                 \
                                                                  bool,                  \
                                                                  std::string_view,      \
                                                                  bool);

T_IMPL_GET_JSON(bool);
T_IMPL_GET_JSON(int32_t);
T_IMPL_GET_JSON(uint32_t);
T_IMPL_GET_JSON(float);
T_IMPL_GET_JSON(double);
T_IMPL_GET_JSON(std::string);
T_IMPL_GET_JSON(std::vector<float>);

template<std::size_t N>
using iarray = std::array<int, N>;
T_IMPL_GET_JSON(iarray<3>);

template<std::size_t N>
using farray = std::array<float, N>;
T_IMPL_GET_JSON(farray<2>);
T_IMPL_GET_JSON(farray<3>);
T_IMPL_GET_JSON(farray<4>);

ScopedJsonUserProperties::ScopedJsonUserProperties(const UserPropertyMap* properties,
                                                   const nlohmann::json*  root)
    : m_previous(g_json_user_properties), m_previous_root(g_json_scene_root) {
    g_json_user_properties = properties;
    g_json_scene_root      = root;
}

ScopedJsonUserProperties::~ScopedJsonUserProperties() {
    g_json_user_properties = m_previous;
    g_json_scene_root      = m_previous_root;
}
} // namespace wallpaper
