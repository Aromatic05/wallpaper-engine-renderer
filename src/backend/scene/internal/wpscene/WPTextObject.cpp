#include "WPTextObject.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>

#include <nlohmann/json.hpp>

#include "resources/WPJson.hpp"
#include "fs/VFS.h"
#include "utils/Logging.h"

using namespace wallpaper::wpscene;

namespace
{
const nlohmann::json* ResolveTextPropertyValueNode(const nlohmann::json& json) {
    if (! json.is_object()) return &json;
    if (json.contains("value") && ! json.at("value").is_null()) return &json.at("value");

    if (json.contains("animation") && json.at("animation").is_object()) {
        const auto& animation = json.at("animation");
        bool        start_paused { false };
        if (animation.contains("options") && animation.at("options").is_object()) {
            GET_JSON_NAME_VALUE_NOWARN(animation.at("options"), "startpaused", start_paused);
        }
        if (start_paused && animation.contains("c0") && animation.at("c0").is_array() &&
            ! animation.at("c0").empty() && animation.at("c0").front().is_object() &&
            animation.at("c0").front().contains("value") &&
            ! animation.at("c0").front().at("value").is_null()) {
            return &animation.at("c0").front().at("value");
        }
    }

    return nullptr;
}

template<typename T>
void ReadLiteralOrDynamicValue(const nlohmann::json& json, const char* name, T* out_value) {
    if (out_value == nullptr || ! json.contains(name) || json.at(name).is_null()) return;

    const auto* value_node = ResolveTextPropertyValueNode(json.at(name));
    if (value_node == nullptr) return;

    GET_JSON_VALUE_NOWARN(*value_node, *out_value);
}

std::array<int32_t, 4> UniformTextPadding(int32_t value) {
    return { value, value, value, value };
}

int32_t MaxTextPaddingEdge(const std::array<int32_t, 4>& padding) {
    return std::max({ padding[0], padding[1], padding[2], padding[3] });
}

std::array<int32_t, 4> ClampTextPaddingEdges(std::array<int32_t, 4> padding) {
    for (auto& edge : padding) edge = std::max(edge, 0);
    return padding;
}

bool ParseTextPaddingComponentString(std::string_view text, std::vector<double>* out_components) {
    if (out_components == nullptr) return false;

    std::istringstream input { std::string(text) };
    std::vector<double> components;
    double              component { 0.0 };
    while (input >> component) components.push_back(component);
    input >> std::ws;
    if (! input.eof() || components.empty()) return false;

    *out_components = std::move(components);
    return true;
}

bool ReadTextPaddingComponents(const nlohmann::json& value_node,
                               std::vector<double>* out_components) {
    if (out_components == nullptr) return false;

    if (value_node.is_number()) {
        *out_components = { value_node.get<double>() };
        return true;
    }
    if (value_node.is_string()) {
        return ParseTextPaddingComponentString(value_node.get_ref<const std::string&>(),
                                               out_components);
    }
    if (! value_node.is_array()) return false;

    std::vector<double> components;
    components.reserve(value_node.size());
    for (const auto& item : value_node) {
        if (item.is_number()) {
            components.push_back(item.get<double>());
        } else if (item.is_string()) {
            std::vector<double> item_components;
            if (! ParseTextPaddingComponentString(item.get_ref<const std::string&>(),
                                                  &item_components)) {
                return false;
            }
            components.insert(components.end(), item_components.begin(), item_components.end());
        } else {
            return false;
        }
    }
    if (components.empty()) return false;
    *out_components = std::move(components);
    return true;
}

int32_t RoundTextPaddingComponent(double value) {
    if (! std::isfinite(value)) return 0;
    return static_cast<int32_t>(std::lround(value));
}

std::optional<std::array<int32_t, 4>> ExpandTextPaddingComponents(
    const std::vector<double>& components) {
    if (components.empty() || components.size() > 4) return std::nullopt;

    const auto edge = [&](size_t index) { return RoundTextPaddingComponent(components[index]); };
    if (components.size() == 1) return UniformTextPadding(edge(0));
    if (components.size() == 2) return { { edge(0), edge(1), edge(0), edge(1) } };
    if (components.size() == 3) return { { edge(0), edge(1), edge(2), edge(1) } };
    return { { edge(0), edge(1), edge(2), edge(3) } };
}

void ReadTextPaddingValue(const nlohmann::json& json, int32_t object_id,
                          int32_t* out_legacy_padding,
                          std::array<int32_t, 4>* out_padding_edges) {
    if (out_legacy_padding == nullptr || out_padding_edges == nullptr ||
        ! json.contains("padding") || json.at("padding").is_null()) {
        return;
    }

    const auto* value_node = ResolveTextPropertyValueNode(json.at("padding"));
    if (value_node == nullptr) return;

    std::vector<double> components;
    if (! ReadTextPaddingComponents(*value_node, &components)) {
        const std::string raw = value_node->dump();
        LOG_ERROR("TextPaddingParse: layer=%d unsupported padding=%s", object_id, raw.c_str());
        return;
    }

    const auto expanded = ExpandTextPaddingComponents(components);
    if (! expanded.has_value()) {
        const std::string raw = value_node->dump();
        LOG_ERROR("TextPaddingParse: layer=%d invalid component-count=%zu padding=%s",
                  object_id,
                  components.size(),
                  raw.c_str());
        return;
    }

    *out_padding_edges  = ClampTextPaddingEdges(*expanded);
    *out_legacy_padding = MaxTextPaddingEdge(*out_padding_edges);
}

void ReadVisibleBinding(const nlohmann::json& json, wallpaper::VisibleBinding* binding) {
    if (binding == nullptr || ! json.is_object()) return;

    GET_JSON_NAME_VALUE_NOWARN(json, "value", binding->value);
    if (! json.contains("user") || json.at("user").is_null()) return;

    const auto& user = json.at("user");
    if (user.is_string()) {
        GET_JSON_VALUE(user, binding->user.name);
        return;
    }
    if (! user.is_object()) return;

    GET_JSON_NAME_VALUE_NOWARN(user, "name", binding->user.name);
    GET_JSON_NAME_VALUE_NOWARN(user, "condition", binding->user.condition);
}

bool PropertyHasScriptOrAnimation(const nlohmann::json& json, const char* name) {
    if (! json.contains(name)) return false;
    const auto& value = json.at(name);
    return value.is_object() &&
           ((value.contains("script") && ! value.at("script").is_null()) ||
            (value.contains("animation") && ! value.at("animation").is_null()));
}
} // namespace

bool WPTextObject::FromJson(const nlohmann::json& json, wallpaper::fs::VFS& vfs) {
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "locktransforms", locktransforms);
    GET_JSON_NAME_VALUE_NOWARN(json, "muteineditor", muteineditor);
    GET_JSON_NAME_VALUE_NOWARN(json, "nointerpolation", nointerpolation);
    ReadJsonIntArray(json, "dependencies", dependencies);
    if (json.contains("instance") && ! json.at("instance").is_null()) {
        instance = json.at("instance");
    }
    GET_JSON_NAME_VALUE_NOWARN(json, "origin", origin);
    GET_JSON_NAME_VALUE_NOWARN(json, "scale", scale);
    GET_JSON_NAME_VALUE_NOWARN(json, "angles", angles);
    GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
    GET_JSON_NAME_VALUE_NOWARN(json, "size", size);
    ReadLiteralOrDynamicValue(json, "text", &text);
    ReadLiteralOrDynamicValue(json, "font", &font);
    ReadLiteralOrDynamicValue(json, "color", &color);
    ReadLiteralOrDynamicValue(json, "backgroundcolor", &backgroundcolor);
    ReadLiteralOrDynamicValue(json, "backgroundbrightness", &backgroundbrightness);
    ReadLiteralOrDynamicValue(json, "alpha", &alpha);
    ReadLiteralOrDynamicValue(json, "pointsize", &pointsize);
    ReadLiteralOrDynamicValue(json, "maxwidth", &maxwidth);
    ReadLiteralOrDynamicValue(json, "maxrows", &maxrows);
    ReadTextPaddingValue(json, id, &padding, &padding_edges);
    GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);
    GET_JSON_NAME_VALUE_NOWARN(json, "attachment", attachment);
    ReadLiteralOrDynamicValue(json, "visible", &visible);
    ReadLiteralOrDynamicValue(json, "opaquebackground", &opaquebackground);
    ReadLiteralOrDynamicValue(json, "blockalign", &blockalign);
    ReadLiteralOrDynamicValue(json, "limitrows", &limitrows);
    ReadLiteralOrDynamicValue(json, "limituseellipsis", &limituseellipsis);
    ReadLiteralOrDynamicValue(json, "limitwidth", &limitwidth);
    GET_JSON_NAME_VALUE_NOWARN(json, "copybackground", copybackground);
    ReadLiteralOrDynamicValue(json, "horizontalalign", &horizontalalign);
    ReadLiteralOrDynamicValue(json, "verticalalign", &verticalalign);
    ReadLiteralOrDynamicValue(json, "anchor", &anchor);
    ReadLiteralOrDynamicValue(json, "depthtest", &depthtest);

    size_explicit = json.contains("size") && ! json.at("size").is_null();
    if (json.contains("visible")) {
        ReadVisibleBinding(json.at("visible"), &visible_binding);
    }
    has_visible_script = json.contains("visible") && json.at("visible").is_object() &&
                         json.at("visible").contains("script") &&
                         ! json.at("visible").at("script").is_null();
    has_dynamic_layout_script =
        PropertyHasScriptOrAnimation(json, "text") || PropertyHasScriptOrAnimation(json, "font") ||
        PropertyHasScriptOrAnimation(json, "pointsize") ||
        PropertyHasScriptOrAnimation(json, "padding") ||
        PropertyHasScriptOrAnimation(json, "maxwidth") ||
        PropertyHasScriptOrAnimation(json, "maxrows") ||
        PropertyHasScriptOrAnimation(json, "limitwidth") ||
        PropertyHasScriptOrAnimation(json, "limitrows") ||
        PropertyHasScriptOrAnimation(json, "horizontalalign") ||
        PropertyHasScriptOrAnimation(json, "verticalalign") ||
        PropertyHasScriptOrAnimation(json, "anchor") || PropertyHasScriptOrAnimation(json, "size");

    if (json.contains("effects") && json.at("effects").is_array()) {
        for (const auto& effect_json : json.at("effects")) {
            WPImageEffect effect;
            if (effect.FromJson(effect_json, vfs)) effects.push_back(std::move(effect));
        }
    }
    return true;
}
