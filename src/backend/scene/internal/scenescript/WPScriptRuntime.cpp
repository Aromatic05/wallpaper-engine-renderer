#include "scenescript/WPScriptRuntime.hpp"

#include <cmath>
#include <sstream>
#include <utility>

#include "utils/Logging.h"

extern "C" {
#include "quickjs.h"
}

namespace wallpaper
{
namespace
{

struct RuntimeState {
    JSRuntime* runtime { nullptr };
    JSContext* context { nullptr };
};

void ReplaceAll(std::string& source, std::string_view needle, std::string_view replacement) {
    std::string::size_type pos = 0;
    while ((pos = source.find(needle, pos)) != std::string::npos) {
        source.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

void NormalizeScriptWhitespace(std::string& source) {
    ReplaceAll(source, "\xC2\xA0", " ");
}

std::string StripScriptModuleSyntax(std::string source) {
    NormalizeScriptWhitespace(source);

    std::string::size_type pos = 0;
    while ((pos = source.find("'use strict';", pos)) != std::string::npos) {
        source.erase(pos, 13);
    }

    pos = 0;
    while ((pos = source.find("\"use strict\";", pos)) != std::string::npos) {
        source.erase(pos, 13);
    }

    pos = 0;
    while ((pos = source.find("export ", pos)) != std::string::npos) {
        source.erase(pos, 7);
    }

    std::istringstream input(source);
    std::ostringstream output;
    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t");
        const auto trimmed =
            first == std::string::npos ? std::string_view {} : std::string_view(line).substr(first);
        if (trimmed.starts_with("import ")) {
            continue;
        }
        output << line << '\n';
    }

    return output.str();
}

void LogQuickJSException(JSContext* context, const char* stage) {
    JSValue exception = JS_GetException(context);

    std::string message = "<unknown>";
    if (const char* text = JS_ToCString(context, exception)) {
        message = text;
        JS_FreeCString(context, text);
    }

    std::string stack;
    JSValue stack_value = JS_GetPropertyStr(context, exception, "stack");
    if (!JS_IsException(stack_value) && !JS_IsUndefined(stack_value)) {
        if (const char* text = JS_ToCString(context, stack_value)) {
            stack = text;
            JS_FreeCString(context, text);
        }
    }
    JS_FreeValue(context, stack_value);

    if (stack.empty()) {
        LOG_INFO("QuickJS %s failed: %s", stage, message.c_str());
    } else {
        LOG_INFO("QuickJS %s failed: %s\n%s", stage, message.c_str(), stack.c_str());
    }

    JS_FreeValue(context, exception);
}

JSValue NumericVectorToJS(JSContext* context, const std::vector<double>& values) {
    static constexpr const char* names[] = { "x", "y", "z", "w" };

    if (values.size() <= 1) {
        return JS_NewFloat64(context, values.empty() ? 0.0 : values.front());
    }

    if (values.size() <= 4) {
        JSValue object = JS_NewObject(context);
        for (size_t index = 0; index < values.size(); index++) {
            JS_SetPropertyStr(context, object, names[index], JS_NewFloat64(context, values[index]));
        }
        return object;
    }

    JSValue array = JS_NewArray(context);
    for (uint32_t index = 0; index < values.size(); index++) {
        JS_SetPropertyUint32(context,
                             array,
                             index,
                             JS_NewFloat64(context, values[static_cast<size_t>(index)]));
    }
    return array;
}

JSValue ScriptValueToJS(JSContext* context, const WPScriptValue& value) {
    switch (value.shape) {
    case WPScriptValueShape::Number:
        return JS_NewFloat64(context, value.numeric_values.empty() ? 0.0 : value.numeric_values.front());
    case WPScriptValueShape::Boolean:
        return JS_NewBool(context, value.boolean_value);
    case WPScriptValueShape::String:
        return JS_NewStringLen(context, value.string_value.c_str(), value.string_value.size());
    case WPScriptValueShape::NumberArray:
    case WPScriptValueShape::VectorString:
        return NumericVectorToJS(context, value.numeric_values);
    }

    return JS_UNDEFINED;
}

bool ReadJSNumber(JSContext* context, JSValueConst value, double* out_value) {
    if (out_value == nullptr) {
        return false;
    }

    if (JS_IsException(value) || JS_IsUndefined(value) || JS_IsNull(value) || JS_IsObject(value)) {
        return false;
    }

    if (JS_ToFloat64(context, out_value, value) != 0) {
        return false;
    }

    return std::isfinite(*out_value);
}

bool ReadJSString(JSContext* context, JSValueConst value, std::string* out_value) {
    if (out_value == nullptr) {
        return false;
    }

    const char* text = JS_ToCString(context, value);
    if (!text) {
        return false;
    }

    *out_value = text;
    JS_FreeCString(context, text);
    return true;
}

bool ReadJSObjectNumber(JSContext* context,
                        JSValueConst value,
                        const char* property_name,
                        double* out_value) {
    JSValue property = JS_GetPropertyStr(context, value, property_name);
    const bool ok = !JS_IsException(property) && !JS_IsUndefined(property) &&
        ReadJSNumber(context, property, out_value);
    JS_FreeValue(context, property);
    return ok;
}

bool ReadJSObjectString(JSContext* context,
                        JSValueConst value,
                        const char* property_name,
                        std::string* out_value) {
    JSValue property = JS_GetPropertyStr(context, value, property_name);
    const bool ok = !JS_IsException(property) && !JS_IsUndefined(property) &&
        ReadJSString(context, property, out_value);
    JS_FreeValue(context, property);
    return ok;
}

bool ReadJSArrayLength(JSContext* context, JSValueConst value, uint32_t* out_length) {
    JSValue length_value = JS_GetPropertyStr(context, value, "length");
    const bool ok = !JS_IsException(length_value) && JS_ToUint32(context, out_length, length_value) == 0;
    JS_FreeValue(context, length_value);
    return ok;
}

bool ReadJSNumericVector(JSContext* context,
                         JSValueConst value,
                         size_t expected_size,
                         std::vector<double>* out_values) {
    if (out_values == nullptr) {
        return false;
    }

    out_values->clear();
    out_values->reserve(expected_size);

    if (expected_size <= 1) {
        double number = 0.0;
        if (!ReadJSNumber(context, value, &number)) {
            return false;
        }
        out_values->push_back(number);
        return true;
    }

    static constexpr const char* names[] = { "x", "y", "z", "w" };
    if (expected_size <= 4 && JS_IsObject(value) && !JS_IsArray(value)) {
        for (size_t index = 0; index < expected_size; index++) {
            double component = 0.0;
            if (!ReadJSObjectNumber(context, value, names[index], &component)) {
                return false;
            }
            out_values->push_back(component);
        }
        return true;
    }

    double scalar = 0.0;
    if (ReadJSNumber(context, value, &scalar)) {
        out_values->assign(expected_size, scalar);
        return true;
    }

    if (!JS_IsArray(value)) {
        return false;
    }

    uint32_t length = 0;
    if (!ReadJSArrayLength(context, value, &length) || length < expected_size) {
        return false;
    }

    for (uint32_t index = 0; index < expected_size; index++) {
        JSValue item = JS_GetPropertyUint32(context, value, index);
        double number = 0.0;
        const bool ok = !JS_IsException(item) && ReadJSNumber(context, item, &number);
        JS_FreeValue(context, item);
        if (!ok) {
            return false;
        }
        out_values->push_back(number);
    }

    return true;
}

std::optional<WPScriptValue> ScriptValueFromJS(JSContext* context,
                                               JSValueConst value,
                                               const WPScriptValue& hint) {
    switch (hint.shape) {
    case WPScriptValueShape::Number: {
        double number = 0.0;
        if (!ReadJSNumber(context, value, &number)) {
            return std::nullopt;
        }
        return WPScriptValue::Number(number);
    }
    case WPScriptValueShape::Boolean: {
        const int result = JS_ToBool(context, value);
        if (result < 0) {
            return std::nullopt;
        }
        return WPScriptValue::Boolean(result != 0);
    }
    case WPScriptValueShape::String: {
        std::string text;
        if (!ReadJSString(context, value, &text)) {
            return std::nullopt;
        }
        return WPScriptValue::String(std::move(text));
    }
    case WPScriptValueShape::NumberArray:
    case WPScriptValueShape::VectorString: {
        std::vector<double> values;
        if (!ReadJSNumericVector(context, value, hint.numeric_values.size(), &values)) {
            return std::nullopt;
        }

        if (hint.shape == WPScriptValueShape::NumberArray) {
            return WPScriptValue::NumberArray(std::move(values));
        }

        return WPScriptValue::VectorString(std::move(values));
    }
    }

    return std::nullopt;
}

JSValue VideoTextureStatesToJS(JSContext* context,
                               const std::vector<WPScriptVideoTextureState>& states) {
    JSValue array = JS_NewArray(context);
    for (uint32_t index = 0; index < states.size(); index++) {
        const auto& state = states[static_cast<size_t>(index)];
        JSValue item = JS_NewObject(context);
        JS_SetPropertyStr(
            context, item, "key", JS_NewStringLen(context, state.key.c_str(), state.key.size()));
        JS_SetPropertyStr(context, item, "paused", JS_NewBool(context, state.paused));
        JS_SetPropertyStr(context, item, "stopped", JS_NewBool(context, state.stopped));
        JS_SetPropertyUint32(context, array, index, item);
    }
    return array;
}

JSValue ColorToJS(JSContext* context, const std::array<float, 3>& color) {
    JSValue array = JS_NewArray(context);
    for (uint32_t i = 0; i < color.size(); i++) {
        JS_SetPropertyUint32(context, array, i, JS_NewFloat64(context, color[i]));
    }
    return array;
}

JSValue MediaStateToJS(JSContext* context, const WPSceneScriptMediaState* state) {
    JSValue object = JS_NewObject(context);
    if (state == nullptr) {
        JS_SetPropertyStr(context, object, "hasThumbnail", JS_NewBool(context, false));
        JS_SetPropertyStr(context, object, "state", JS_NewInt32(context, 0));
        return object;
    }

    JS_SetPropertyStr(context, object, "hasThumbnail", JS_NewBool(context, state->has_thumbnail));
    JS_SetPropertyStr(context, object, "state", JS_NewInt32(context, state->playback_state));
    JS_SetPropertyStr(context, object, "primaryColor", ColorToJS(context, state->primary_color));
    JS_SetPropertyStr(context, object, "secondaryColor", ColorToJS(context, state->secondary_color));
    JS_SetPropertyStr(context, object, "tertiaryColor", ColorToJS(context, state->tertiary_color));
    JS_SetPropertyStr(context, object, "textColor", ColorToJS(context, state->text_color));
    JS_SetPropertyStr(context, object, "highContrastColor", ColorToJS(context, state->high_contrast_color));
    JS_SetPropertyStr(context, object, "title", JS_NewString(context, state->title.c_str()));
    JS_SetPropertyStr(context, object, "artist", JS_NewString(context, state->artist.c_str()));
    JS_SetPropertyStr(context, object, "albumTitle", JS_NewString(context, state->album_title.c_str()));
    JS_SetPropertyStr(context, object, "albumArtist", JS_NewString(context, state->album_artist.c_str()));
    JS_SetPropertyStr(context, object, "subTitle", JS_NewString(context, state->sub_title.c_str()));
    JS_SetPropertyStr(context, object, "genres", JS_NewString(context, state->genres.c_str()));
    JS_SetPropertyStr(context, object, "contentType", JS_NewString(context, state->content_type.c_str()));
    return object;
}

void ReadVideoTextureEventsFromJS(JSContext* context,
                                  JSValueConst global,
                                  std::vector<WPScriptVideoTextureEvent>* out_events) {
    if (out_events == nullptr) {
        return;
    }

    JSValue events = JS_GetPropertyStr(context, global, "__videoTextureEvents");
    if (JS_IsException(events) || !JS_IsArray(events)) {
        JS_FreeValue(context, events);
        return;
    }

    uint32_t length = 0;
    if (!ReadJSArrayLength(context, events, &length)) {
        JS_FreeValue(context, events);
        return;
    }

    for (uint32_t index = 0; index < length; index++) {
        JSValue item = JS_GetPropertyUint32(context, events, index);
        if (JS_IsException(item) || !JS_IsObject(item)) {
            JS_FreeValue(context, item);
            continue;
        }

        WPScriptVideoTextureEvent event;
        const bool has_key = ReadJSObjectString(context, item, "key", &event.key);
        const bool has_method = ReadJSObjectString(context, item, "method", &event.method);
        if (has_key && has_method && !event.key.empty() && !event.method.empty()) {
            double current_time = 0.0;
            if (ReadJSObjectNumber(context, item, "currentTime", &current_time)) {
                event.current_time = current_time;
            }
            out_events->push_back(std::move(event));
        }

        JS_FreeValue(context, item);
    }

    JS_FreeValue(context, events);
}

std::string BuildWrappedScript(std::string_view script_source) {
    const std::string body = StripScriptModuleSyntax(std::string(script_source));

    std::ostringstream wrapper;
    wrapper << "(function() {\n"
            << "  const __props = globalThis.__scriptProps;\n"
            << "  const __rawInitialValue = globalThis.__currentValue;\n"
            << "  const __propertyName = String(globalThis.__propertyName ?? '');\n"
            << "  const __seedShared = (globalThis.shared && typeof globalThis.shared === 'object')\n"
            << "    ? globalThis.shared\n"
            << "    : {};\n"
            << "  const __sharedState = (globalThis.__sharedState && typeof globalThis.__sharedState === 'object')\n"
            << "    ? globalThis.__sharedState\n"
            << "    : (globalThis.__sharedState = { value: __seedShared });\n"
            << "  if (!__sharedState.value || typeof __sharedState.value !== 'object') {\n"
            << "    __sharedState.value = __seedShared;\n"
            << "  }\n"
            << "  Object.defineProperty(globalThis, 'shared', {\n"
            << "    configurable: true,\n"
            << "    enumerable: true,\n"
            << "    get() {\n"
            << "      return __sharedState.value;\n"
            << "    },\n"
            << "    set(value) {\n"
            << "      const nextValue = value && typeof value === 'object' ? value : {};\n"
            << "      const currentValue = __sharedState.value && typeof __sharedState.value === 'object'\n"
            << "        ? __sharedState.value\n"
            << "        : (__sharedState.value = {});\n"
            << "      for (const key of Object.keys(currentValue)) {\n"
            << "        delete currentValue[key];\n"
            << "      }\n"
            << "      Object.assign(currentValue, nextValue);\n"
            << "    }\n"
            << "  });\n"
            << "  const console = (globalThis.console && typeof globalThis.console === 'object')\n"
            << "    ? globalThis.console\n"
            << "    : (globalThis.console = {\n"
            << "        log() {},\n"
            << "        info() {},\n"
            << "        warn() {},\n"
            << "        error() {}\n"
            << "      });\n"
            << "  const __toNumber = (value, fallback = 0) => {\n"
            << "    const number = Number(value);\n"
            << "    return Number.isFinite(number) ? number : fallback;\n"
            << "  };\n"
            << "  const __vecValues = (value, size) => {\n"
            << "    if (value === undefined || value === null) return Array.from({ length: size }, () => 0);\n"
            << "    if (Array.isArray(value)) {\n"
            << "      return Array.from({ length: size }, (_, index) => __toNumber(value[index], 0));\n"
            << "    }\n"
            << "    if (typeof value === 'number') {\n"
            << "      return Array.from({ length: size }, () => __toNumber(value, 0));\n"
            << "    }\n"
            << "    if (typeof value === 'object') {\n"
            << "      const names = ['x', 'y', 'z', 'w'];\n"
            << "      return Array.from({ length: size }, (_, index) => __toNumber(value[names[index]], 0));\n"
            << "    }\n"
            << "    return Array.from({ length: size }, () => 0);\n"
            << "  };\n"
            << "  const __binaryVec = (lhs, rhs, op, Ctor, names) => {\n"
            << "    const left = names.map((name) => __toNumber(lhs[name], 0));\n"
            << "    const right = __vecValues(rhs, names.length);\n"
            << "    return new Ctor(...left.map((value, index) => op(value, right[index])));\n"
            << "  };\n"
            << "  const __dot = (lhs, rhs) => lhs.reduce((sum, value, index) => sum + value * rhs[index], 0);\n"
            << "  const __mixScalar = (a, b, t) => __toNumber(a, 0) + (__toNumber(b, 0) - __toNumber(a, 0)) * __toNumber(t, 0);\n"
            << "  const Vec2 = (typeof globalThis.Vec2 === 'function')\n"
            << "    ? globalThis.Vec2\n"
            << "    : (globalThis.Vec2 = class Vec2 {\n"
            << "        constructor(a = 0, b = 0) {\n"
            << "          const values = arguments.length === 0 ? [0, 0]\n"
            << "            : arguments.length === 1 ? __vecValues(a, 2)\n"
            << "            : [__toNumber(a, 0), __toNumber(b, 0)];\n"
            << "          this.x = values[0];\n"
            << "          this.y = values[1];\n"
            << "        }\n"
            << "        equals(other) { const v = __vecValues(other, 2); return Math.abs(this.x - v[0]) < 1e-6 && Math.abs(this.y - v[1]) < 1e-6; }\n"
            << "        length() { return Math.hypot(this.x, this.y); }\n"
            << "        lengthSqr() { return this.x * this.x + this.y * this.y; }\n"
            << "        normalize() { const len = this.length(); return len === 0 ? new Vec2() : new Vec2(this.x / len, this.y / len); }\n"
            << "        copy() { return new Vec2(this.x, this.y); }\n"
            << "        add(value) { return __binaryVec(this, value, (a, b) => a + b, Vec2, ['x', 'y']); }\n"
            << "        subtract(value) { return __binaryVec(this, value, (a, b) => a - b, Vec2, ['x', 'y']); }\n"
            << "        multiply(value) { return __binaryVec(this, value, (a, b) => a * b, Vec2, ['x', 'y']); }\n"
            << "        divide(value) { return __binaryVec(this, value, (a, b) => a / b, Vec2, ['x', 'y']); }\n"
            << "        dot(value) { const rhs = __vecValues(value, 2); return __dot([this.x, this.y], rhs); }\n"
            << "        reflect(normal) { const n = new Vec2(normal).normalize(); return this.subtract(n.multiply(2 * this.dot(n))); }\n"
            << "        mix(other, amount) { const rhs = __vecValues(other, 2); return new Vec2(__mixScalar(this.x, rhs[0], amount), __mixScalar(this.y, rhs[1], amount)); }\n"
            << "        perpendicular() { return new Vec2(-this.y, this.x); }\n"
            << "        abs() { return new Vec2(Math.abs(this.x), Math.abs(this.y)); }\n"
            << "        sign() { return new Vec2(Math.sign(this.x), Math.sign(this.y)); }\n"
            << "        round() { return new Vec2(Math.round(this.x), Math.round(this.y)); }\n"
            << "        floor() { return new Vec2(Math.floor(this.x), Math.floor(this.y)); }\n"
            << "        ceil() { return new Vec2(Math.ceil(this.x), Math.ceil(this.y)); }\n"
            << "        toString() { return `${this.x} ${this.y}`; }\n"
            << "      });\n"
            << "  const Vec3 = (typeof globalThis.Vec3 === 'function')\n"
            << "    ? globalThis.Vec3\n"
            << "    : (globalThis.Vec3 = class Vec3 {\n"
            << "        constructor(a = 0, b = 0, c = 0) {\n"
            << "          const values = arguments.length === 0 ? [0, 0, 0]\n"
            << "            : arguments.length === 1 ? __vecValues(a, 3)\n"
            << "            : arguments.length === 2 ? [__toNumber(a, 0), __toNumber(b, 0), 0]\n"
            << "            : [__toNumber(a, 0), __toNumber(b, 0), __toNumber(c, 0)];\n"
            << "          this.x = values[0];\n"
            << "          this.y = values[1];\n"
            << "          this.z = values[2];\n"
            << "        }\n"
            << "        equals(other) { const v = __vecValues(other, 3); return Math.abs(this.x - v[0]) < 1e-6 && Math.abs(this.y - v[1]) < 1e-6 && Math.abs(this.z - v[2]) < 1e-6; }\n"
            << "        length() { return Math.hypot(this.x, this.y, this.z); }\n"
            << "        lengthSqr() { return this.x * this.x + this.y * this.y + this.z * this.z; }\n"
            << "        normalize() { const len = this.length(); return len === 0 ? new Vec3() : new Vec3(this.x / len, this.y / len, this.z / len); }\n"
            << "        copy() { return new Vec3(this.x, this.y, this.z); }\n"
            << "        add(value) { return __binaryVec(this, value, (a, b) => a + b, Vec3, ['x', 'y', 'z']); }\n"
            << "        subtract(value) { return __binaryVec(this, value, (a, b) => a - b, Vec3, ['x', 'y', 'z']); }\n"
            << "        multiply(value) { return __binaryVec(this, value, (a, b) => a * b, Vec3, ['x', 'y', 'z']); }\n"
            << "        divide(value) { return __binaryVec(this, value, (a, b) => a / b, Vec3, ['x', 'y', 'z']); }\n"
            << "        dot(value) { const rhs = __vecValues(value, 3); return __dot([this.x, this.y, this.z], rhs); }\n"
            << "        reflect(normal) { const n = new Vec3(normal).normalize(); return this.subtract(n.multiply(2 * this.dot(n))); }\n"
            << "        mix(other, amount) { const rhs = __vecValues(other, 3); return new Vec3(__mixScalar(this.x, rhs[0], amount), __mixScalar(this.y, rhs[1], amount), __mixScalar(this.z, rhs[2], amount)); }\n"
            << "        min(value) { return __binaryVec(this, value, (a, b) => Math.min(a, b), Vec3, ['x', 'y', 'z']); }\n"
            << "        max(value) { return __binaryVec(this, value, (a, b) => Math.max(a, b), Vec3, ['x', 'y', 'z']); }\n"
            << "        cross(value) { const rhs = __vecValues(value, 3); return new Vec3(this.y * rhs[2] - this.z * rhs[1], this.z * rhs[0] - this.x * rhs[2], this.x * rhs[1] - this.y * rhs[0]); }\n"
            << "        abs() { return new Vec3(Math.abs(this.x), Math.abs(this.y), Math.abs(this.z)); }\n"
            << "        sign() { return new Vec3(Math.sign(this.x), Math.sign(this.y), Math.sign(this.z)); }\n"
            << "        round() { return new Vec3(Math.round(this.x), Math.round(this.y), Math.round(this.z)); }\n"
            << "        floor() { return new Vec3(Math.floor(this.x), Math.floor(this.y), Math.floor(this.z)); }\n"
            << "        ceil() { return new Vec3(Math.ceil(this.x), Math.ceil(this.y), Math.ceil(this.z)); }\n"
            << "        toString() { return `${this.x} ${this.y} ${this.z}`; }\n"
            << "      });\n"
            << "  const __wrapScriptValue = (value) => {\n"
            << "    if (value === undefined || value === null) return value;\n"
            << "    if (typeof value !== 'object') return value;\n"
            << "    if (typeof value.x === 'number' && typeof value.y === 'number' && typeof value.z === 'number') {\n"
            << "      return new Vec3(value);\n"
            << "    }\n"
            << "    if (typeof value.x === 'number' && typeof value.y === 'number') {\n"
            << "      return new Vec2(value);\n"
            << "    }\n"
            << "    return value;\n"
            << "  };\n"
            << "  const MediaPlaybackEvent = globalThis.MediaPlaybackEvent ?? (globalThis.MediaPlaybackEvent = Object.freeze({\n"
            << "    PLAYBACK_STOPPED: 0,\n"
            << "    PLAYBACK_PLAYING: 1,\n"
            << "    PLAYBACK_PAUSED: 2,\n"
            << "    PLAYBACK_OTHER: 3\n"
            << "  }));\n"
            << "  const __makeNoopAnimation = () => ({ play() {}, stop() {}, pause() {}, isPlaying() { return false; }, getFrame() { return 0; }, setFrame() {}, join() {}, addEndedCallback() {} });\n"
            << "  const __makeNoopVideoTexture = () => ({ play() {}, pause() {}, stop() {}, setCurrentTime() {}, isPlaying() { return false; } });\n"
            << "  const __videoTextureStates = Array.isArray(globalThis.__videoTextureStates)\n"
            << "    ? globalThis.__videoTextureStates\n"
            << "    : [];\n"
            << "  const __videoTextureEvents = [];\n"
            << "  globalThis.__videoTextureEvents = __videoTextureEvents;\n"
            << "  const __videoTextureStateByKey = new Map(__videoTextureStates.map((state) => [String(state.key ?? ''), {\n"
            << "    paused: !!state.paused,\n"
            << "    stopped: !!state.stopped\n"
            << "  }]));\n"
            << "  const __makeVideoTexture = (key) => {\n"
            << "    const textureKey = String(key ?? '');\n"
            << "    if (!textureKey) return __makeNoopVideoTexture();\n"
            << "    const state = __videoTextureStateByKey.get(textureKey) ?? { paused: false, stopped: false };\n"
            << "    const record = (method, currentTime = 0) => {\n"
            << "      const event = { key: textureKey, method };\n"
            << "      if (method === 'setCurrentTime') event.currentTime = Math.max(0, __toNumber(currentTime, 0));\n"
            << "      __videoTextureEvents.push(event);\n"
            << "      if (method === 'play') { state.paused = false; state.stopped = false; }\n"
            << "      else if (method === 'pause') { state.paused = true; state.stopped = false; }\n"
            << "      else if (method === 'stop') { state.paused = true; state.stopped = true; }\n"
            << "    };\n"
            << "    return {\n"
            << "      play() { record('play'); },\n"
            << "      pause() { record('pause'); },\n"
            << "      stop() { record('stop'); },\n"
            << "      setCurrentTime(value) { record('setCurrentTime', value); },\n"
            << "      isPlaying() { return !state.paused && !state.stopped; }\n"
            << "    };\n"
            << "  };\n"
            << "  const __makeLayerVideoTexture = () => __videoTextureStates.length > 0\n"
            << "    ? __makeVideoTexture(__videoTextureStates[0].key)\n"
            << "    : __makeNoopVideoTexture();\n"
            << "  const __makeIdentityTransformMatrix = () => ({ m: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] });\n"
            << "  const WEMath = (globalThis.WEMath && typeof globalThis.WEMath === 'object')\n"
            << "    ? globalThis.WEMath\n"
            << "    : (globalThis.WEMath = {\n"
            << "        deg2rad: Math.PI / 180,\n"
            << "        rad2deg: 180 / Math.PI,\n"
            << "        mix(a, b, t) {\n"
            << "          const lerp = (x, y, alpha) => Number(x) + (Number(y) - Number(x)) * Number(alpha);\n"
            << "          if (Array.isArray(a) && Array.isArray(b)) {\n"
            << "            return a.map((value, index) => lerp(value, b[index] ?? value, t));\n"
            << "          }\n"
            << "          if (a && b && typeof a === 'object' && typeof b === 'object') {\n"
            << "            const out = Array.isArray(a) ? [] : {};\n"
            << "            for (const key of Object.keys(a)) {\n"
            << "              out[key] = lerp(a[key], b[key] ?? a[key], t);\n"
            << "            }\n"
            << "            return out;\n"
            << "          }\n"
            << "          return lerp(a, b, t);\n"
            << "        },\n"
            << "        smoothStep(minValue, maxValue, value) {\n"
            << "          const edge0 = __toNumber(minValue, 0);\n"
            << "          const edge1 = __toNumber(maxValue, 1);\n"
            << "          if (edge0 === edge1) return value < edge0 ? 0 : 1;\n"
            << "          const x = Math.min(Math.max((__toNumber(value, 0) - edge0) / (edge1 - edge0), 0), 1);\n"
            << "          return x * x * (3 - 2 * x);\n"
            << "        },\n"
            << "        clamp(value, minValue, maxValue) {\n"
            << "          return Math.min(Math.max(Number(value), Number(minValue)), Number(maxValue));\n"
            << "        }\n"
            << "      });\n"
            << "  const WEColor = (globalThis.WEColor && typeof globalThis.WEColor === 'object')\n"
            << "    ? globalThis.WEColor\n"
            << "    : (globalThis.WEColor = {\n"
            << "        rgb2hsv(rgb) {\n"
            << "          const [r, g, b] = __vecValues(rgb, 3);\n"
            << "          const max = Math.max(r, g, b);\n"
            << "          const min = Math.min(r, g, b);\n"
            << "          const delta = max - min;\n"
            << "          let h = 0;\n"
            << "          if (delta !== 0) {\n"
            << "            if (max === r) h = ((g - b) / delta) % 6;\n"
            << "            else if (max === g) h = (b - r) / delta + 2;\n"
            << "            else h = (r - g) / delta + 4;\n"
            << "            h /= 6;\n"
            << "            if (h < 0) h += 1;\n"
            << "          }\n"
            << "          const s = max === 0 ? 0 : delta / max;\n"
            << "          return new Vec3(h, s, max);\n"
            << "        },\n"
            << "        hsv2rgb(hsv) {\n"
            << "          let [h, s, v] = __vecValues(hsv, 3);\n"
            << "          h = ((h % 1) + 1) % 1;\n"
            << "          const i = Math.floor(h * 6);\n"
            << "          const f = h * 6 - i;\n"
            << "          const p = v * (1 - s);\n"
            << "          const q = v * (1 - f * s);\n"
            << "          const t = v * (1 - (1 - f) * s);\n"
            << "          switch (i % 6) {\n"
            << "            case 0: return new Vec3(v, t, p);\n"
            << "            case 1: return new Vec3(q, v, p);\n"
            << "            case 2: return new Vec3(p, v, t);\n"
            << "            case 3: return new Vec3(p, q, v);\n"
            << "            case 4: return new Vec3(t, p, v);\n"
            << "            default: return new Vec3(v, p, q);\n"
            << "          }\n"
            << "        },\n"
            << "        normalizeColor(rgb) { const [r, g, b] = __vecValues(rgb, 3); return new Vec3(r / 255, g / 255, b / 255); },\n"
            << "        expandColor(rgb) { const [r, g, b] = __vecValues(rgb, 3); return new Vec3(r * 255, g * 255, b * 255); }\n"
            << "      });\n"
            << "  const WEVector = (globalThis.WEVector && typeof globalThis.WEVector === 'object')\n"
            << "    ? globalThis.WEVector\n"
            << "    : (globalThis.WEVector = {\n"
            << "        angleVector2(angle) {\n"
            << "          const radians = __toNumber(angle, 0) * WEMath.deg2rad;\n"
            << "          return new Vec2(Math.cos(radians), Math.sin(radians));\n"
            << "        },\n"
            << "        vectorAngle2(direction) {\n"
            << "          const [x, y] = __vecValues(direction, 2);\n"
            << "          return Math.atan2(y, x) * WEMath.rad2deg;\n"
            << "        }\n"
            << "      });\n"
            << "  const __initialValue = __wrapScriptValue(__rawInitialValue);\n"
            << "  const engine = (globalThis.engine && typeof globalThis.engine === 'object')\n"
            << "    ? globalThis.engine\n"
            << "    : {};\n"
            << "  if (typeof engine.isRunningInEditor !== 'function') engine.isRunningInEditor = () => false;\n"
            << "  if (typeof engine.registerAsset !== 'function') engine.registerAsset = (file) => ({ file: String(file ?? '') });\n"
            << "  if (typeof engine.registerAudioBuffers !== 'function') {\n"
            << "    engine.registerAudioBuffers = (resolution) => ({\n"
            << "      average: Array.from({ length: Math.max(0, Number(resolution) || 0) }, () => 0)\n"
            << "    });\n"
            << "  }\n"
            << "  if (typeof engine.frametime !== 'number') engine.frametime = 1 / 60;\n"
            << "  const __timerState = (globalThis.__timerState && typeof globalThis.__timerState === 'object')\n"
            << "    ? globalThis.__timerState\n"
            << "    : (globalThis.__timerState = { now: 0, nextId: 1, timers: [] });\n"
            << "  __timerState.now += Math.max(0, __toNumber(engine.frametime, 1 / 60));\n"
            << "  engine.setTimeout = (callback, delay = 0) => {\n"
            << "    if (typeof callback !== 'function') return 0;\n"
            << "    const id = __timerState.nextId++;\n"
            << "    const seconds = Math.max(0, __toNumber(delay, 0)) / 1000;\n"
            << "    __timerState.timers.push({ id, callback, due: __timerState.now + seconds });\n"
            << "    return id;\n"
            << "  };\n"
            << "  engine.clearTimeout = (timer) => {\n"
            << "    const id = Number(timer);\n"
            << "    __timerState.timers = __timerState.timers.filter((entry) => entry.id !== id);\n"
            << "  };\n"
            << "  engine.clearInterval = engine.clearTimeout;\n"
            << "  const __dispatchDueTimers = () => {\n"
            << "    const due = [];\n"
            << "    const pending = [];\n"
            << "    for (const timer of __timerState.timers) {\n"
            << "      if (timer && timer.due <= __timerState.now && due.length < 16) due.push(timer);\n"
            << "      else pending.push(timer);\n"
            << "    }\n"
            << "    __timerState.timers = pending;\n"
            << "    for (const timer of due) {\n"
            << "      try { timer.callback(); } catch (__timerError) {}\n"
            << "    }\n"
            << "  };\n"
            << "  __dispatchDueTimers();\n"
            << "  if (engine.AUDIO_RESOLUTION_16 === undefined) engine.AUDIO_RESOLUTION_16 = 16;\n"
            << "  if (!engine.userProperties || typeof engine.userProperties !== 'object') engine.userProperties = {};\n"
            << "  globalThis.engine = engine;\n"
            << "  const __localStorageState = (globalThis.__localStorageState && typeof globalThis.__localStorageState === 'object')\n"
            << "    ? globalThis.__localStorageState\n"
            << "    : (globalThis.__localStorageState = { global: {}, screen: {} });\n"
            << "  const __resolveLocalStorageBucket = (location = 'screen') => {\n"
            << "    const key = String(location) === 'global' ? 'global' : 'screen';\n"
            << "    const bucket = __localStorageState[key];\n"
            << "    if (!bucket || typeof bucket !== 'object') __localStorageState[key] = {};\n"
            << "    return __localStorageState[key];\n"
            << "  };\n"
            << "  globalThis.localStorage = {\n"
            << "    LOCATION_GLOBAL: 'global',\n"
            << "    LOCATION_SCREEN: 'screen',\n"
            << "    get(key, location = 'screen') { return __resolveLocalStorageBucket(location)[String(key)]; },\n"
            << "    set(key, value, location = 'screen') { __resolveLocalStorageBucket(location)[String(key)] = value; },\n"
            << "    clear(location = 'screen') {\n"
            << "      const key = String(location) === 'global' ? 'global' : 'screen';\n"
            << "      __localStorageState[key] = {};\n"
            << "    },\n"
            << "    delete(key, location = 'screen') {\n"
            << "      const bucket = __resolveLocalStorageBucket(location);\n"
            << "      const storageKey = String(key);\n"
            << "      const had = Object.prototype.hasOwnProperty.call(bucket, storageKey);\n"
            << "      delete bucket[storageKey];\n"
            << "      return had;\n"
            << "    },\n"
            << "    remove(key, location = 'screen') {\n"
            << "      return this.delete(key, location);\n"
            << "    }\n"
            << "  };\n"
            << "  const thisObject = (globalThis.thisObject && typeof globalThis.thisObject === 'object')\n"
            << "    ? globalThis.thisObject\n"
            << "    : (globalThis.thisObject = {});\n"
            << "  let thisLayer = (globalThis.thisLayer && typeof globalThis.thisLayer === 'object')\n"
            << "    ? globalThis.thisLayer\n"
            << "    : (globalThis.thisLayer = {});\n"
            << "  if (__propertyName) {\n"
            << "    thisLayer[__propertyName] = __initialValue;\n"
            << "  }\n"
            << "  if (thisLayer.origin === undefined) thisLayer.origin = new Vec3();\n"
            << "  if (thisLayer.scale === undefined) thisLayer.scale = new Vec3(1, 1, 1);\n"
            << "  if (thisLayer.angles === undefined) thisLayer.angles = new Vec3();\n"
            << "  if (__propertyName === 'origin' || thisLayer.originalOrigin === undefined) thisLayer.originalOrigin = new Vec3(thisLayer.origin);\n"
            << "  if (thisLayer.size === undefined) thisLayer.size = new Vec2();\n"
            << "  if (thisLayer.visible === undefined) thisLayer.visible = typeof __initialValue === 'boolean' ? __initialValue : true;\n"
            << "  if (typeof thisObject.getMaterial !== 'function') thisObject.getMaterial = () => ({});\n"
            << "  if (typeof thisObject.getEffect !== 'function') thisObject.getEffect = () => ({});\n"
            << "  if (typeof thisObject.getAnimation !== 'function') thisObject.getAnimation = () => __makeNoopAnimation();\n"
            << "  if (typeof thisLayer.getTransformMatrix !== 'function') thisLayer.getTransformMatrix = __makeIdentityTransformMatrix;\n"
            << "  if (typeof thisLayer.getParent !== 'function') thisLayer.getParent = () => thisLayer;\n"
            << "  if (typeof thisLayer.getTextureAnimation !== 'function') thisLayer.getTextureAnimation = () => __makeNoopAnimation();\n"
            << "  if (typeof thisLayer.getAnimation !== 'function') thisLayer.getAnimation = () => __makeNoopAnimation();\n"
            << "  if (typeof thisLayer.getAnimationLayer !== 'function') thisLayer.getAnimationLayer = () => __makeNoopAnimation();\n"
            << "  thisLayer.getVideoTexture = __makeLayerVideoTexture;\n"
            << "  const __sceneLayerStore = (globalThis.__sceneLayerStore && typeof globalThis.__sceneLayerStore.get === 'function')\n"
            << "    ? globalThis.__sceneLayerStore\n"
            << "    : (globalThis.__sceneLayerStore = new Map());\n"
            << "  const __getSceneLayerStub = (name) => {\n"
            << "    const key = String(name ?? '');\n"
            << "    if (__sceneLayerStore.has(key)) return __sceneLayerStore.get(key);\n"
            << "    const target = {};\n"
            << "    const proxy = new Proxy(target, {\n"
            << "      get(obj, prop, receiver) {\n"
            << "        if (prop === 'getLayer') return __getSceneLayerStub;\n"
            << "        if (prop === 'getLayerIndex') return () => -1;\n"
            << "        if (prop === 'getInitialLayerConfig') return () => ({});\n"
            << "        if (prop === 'getTransformMatrix') return __makeIdentityTransformMatrix;\n"
            << "        if (prop === 'getParent') return () => proxy;\n"
            << "        if (prop === 'getTextureAnimation' || prop === 'getAnimation' || prop === 'getAnimationLayer') return () => __makeNoopAnimation();\n"
            << "        if (prop === 'getVideoTexture') return () => __makeNoopVideoTexture();\n"
            << "        if (prop === 'destroyLayer' || prop === 'sortLayer') return () => {};\n"
            << "        return Reflect.get(obj, prop, receiver);\n"
            << "      },\n"
            << "      set(obj, prop, value) {\n"
            << "        obj[prop] = value;\n"
            << "        return true;\n"
            << "      }\n"
            << "    });\n"
            << "    __sceneLayerStore.set(key, proxy);\n"
            << "    return proxy;\n"
            << "  };\n"
            << "  const __sceneTarget = (globalThis.thisScene && typeof globalThis.thisScene === 'object')\n"
            << "    ? globalThis.thisScene\n"
            << "    : {};\n"
            << "  if (typeof __sceneTarget.getLayer !== 'function') __sceneTarget.getLayer = __getSceneLayerStub;\n"
            << "  if (typeof __sceneTarget.getLayerCount !== 'function') __sceneTarget.getLayerCount = () => 0;\n"
            << "  if (typeof __sceneTarget.enumerateLayers !== 'function') __sceneTarget.enumerateLayers = () => [];\n"
            << "  if (typeof __sceneTarget.createLayer !== 'function') __sceneTarget.createLayer = (name) => __getSceneLayerStub(name);\n"
            << "  if (typeof __sceneTarget.destroyLayer !== 'function') __sceneTarget.destroyLayer = () => {};\n"
            << "  if (typeof __sceneTarget.sortLayer !== 'function') __sceneTarget.sortLayer = () => {};\n"
            << "  if (typeof __sceneTarget.getLayerIndex !== 'function') __sceneTarget.getLayerIndex = () => -1;\n"
            << "  if (typeof __sceneTarget.getInitialLayerConfig !== 'function') __sceneTarget.getInitialLayerConfig = () => ({});\n"
            << "  if (typeof __sceneTarget.on !== 'function') __sceneTarget.on = () => {};\n"
            << "  globalThis.thisScene = __sceneTarget;\n"
            << "  const thisScene = globalThis.thisScene;\n"
            << "  const scene = thisScene;\n"
            << "  const input = (globalThis.input && typeof globalThis.input === 'object')\n"
            << "    ? globalThis.input\n"
            << "    : (globalThis.input = {});\n"
            << "  if (!input.cursorWorldPosition || typeof input.cursorWorldPosition !== 'object') input.cursorWorldPosition = new Vec3();\n"
            << "  if (!input.cursorScreenPosition || typeof input.cursorScreenPosition !== 'object') input.cursorScreenPosition = new Vec2();\n"
            << "  globalThis.input = input;\n"
            << "  function createScriptProperties() {\n"
            << "    const applyOption = function(opts) {\n"
            << "      if (opts && opts.name !== undefined && !Object.prototype.hasOwnProperty.call(__props, opts.name)) {\n"
            << "        let value = opts.value;\n"
            << "        if (value === undefined && Array.isArray(opts.options) && opts.options.length > 0) {\n"
            << "          const first = opts.options[0] ?? {};\n"
            << "          value = first.value !== undefined ? first.value : first.label;\n"
            << "        }\n"
            << "        __props[opts.name] = value;\n"
            << "      }\n"
            << "      return builder;\n"
            << "    };\n"
            << "    const target = {\n"
            << "      finish() { return __props; }\n"
            << "    };\n"
            << "    const builder = new Proxy(target, {\n"
            << "      get(obj, prop, receiver) {\n"
            << "        if (prop in obj) return Reflect.get(obj, prop, receiver);\n"
            << "        if (typeof prop === 'string' && prop.startsWith('add')) return applyOption;\n"
            << "        return undefined;\n"
            << "      }\n"
            << "    });\n"
            << "    return builder;\n"
            << "  }\n"
            << body << "\n"
            << "  const __mediaState = (globalThis.__mediaState && typeof globalThis.__mediaState === 'object') ? globalThis.__mediaState : {};\n"
            << "  const __dispatchMediaEvents = () => {\n"
            << "    if (globalThis.__dispatchMediaThumbnail && typeof mediaThumbnailChanged === 'function') {\n"
            << "      try { mediaThumbnailChanged({ hasThumbnail: !!__mediaState.hasThumbnail, primaryColor: __mediaState.primaryColor ?? [0, 0, 0], secondaryColor: __mediaState.secondaryColor ?? [1, 1, 1], tertiaryColor: __mediaState.tertiaryColor ?? [1, 1, 1], textColor: __mediaState.textColor ?? [1, 1, 1], highContrastColor: __mediaState.highContrastColor ?? [1, 1, 1] }); } catch (__mediaError) {}\n"
            << "    }\n"
            << "    if (globalThis.__dispatchMediaProperties && typeof mediaPropertiesChanged === 'function') {\n"
            << "      try { mediaPropertiesChanged({ title: String(__mediaState.title ?? ''), artist: String(__mediaState.artist ?? ''), albumTitle: String(__mediaState.albumTitle ?? ''), albumArtist: String(__mediaState.albumArtist ?? ''), subTitle: String(__mediaState.subTitle ?? ''), genres: String(__mediaState.genres ?? ''), contentType: String(__mediaState.contentType ?? '') }); } catch (__mediaError) {}\n"
            << "    }\n"
            << "    if (globalThis.__dispatchMediaPlayback && typeof mediaPlaybackChanged === 'function') {\n"
            << "      try { mediaPlaybackChanged({ state: Number(__mediaState.state ?? 0) }); } catch (__mediaError) {}\n"
            << "    }\n"
            << "  };\n"
            << "  __dispatchMediaEvents();\n"
            << "  const __ensureUpdateValue = (value) => (value === undefined || value === null) ? new Vec3() : value;\n"
            << "  let __result = __ensureUpdateValue(__initialValue);\n"
            << "  if (typeof init === 'function') {\n"
            << "    try {\n"
            << "      const __initResult = init(__result);\n"
            << "      if (__initResult !== undefined) __result = __ensureUpdateValue(__wrapScriptValue(__initResult));\n"
            << "    } catch (__probeError) {}\n"
            << "  }\n"
            << "  if (typeof applyUserProperties === 'function') {\n"
            << "    try {\n"
            << "      const __changedUserProperties = {};\n"
            << "      if (__props && typeof __props === 'object') {\n"
            << "        Object.assign(__changedUserProperties, __props);\n"
            << "      }\n"
            << "      applyUserProperties(__changedUserProperties);\n"
            << "    } catch (__probeError) {}\n"
            << "  }\n"
            << "  if (typeof update === 'function') {\n"
            << "    try {\n"
            << "      const __updatedResult = update(__ensureUpdateValue(__result));\n"
            << "      if (__updatedResult !== undefined) __result = __wrapScriptValue(__updatedResult);\n"
            << "    } catch (__probeError) {}\n"
            << "  }\n"
            << "  return __result;\n"
            << "})();\n";
    return wrapper.str();
}

void InstallGlobalValue(JSContext* context, JSValueConst global, const char* name, JSValue value) {
    JS_SetPropertyStr(context, global, name, value);
}

} // namespace

struct WPScriptRuntime::Opaque {
    RuntimeState state;
};

WPScriptValue WPScriptValue::Number(double value) {
    WPScriptValue result;
    result.shape = WPScriptValueShape::Number;
    result.numeric_values = { value };
    return result;
}

WPScriptValue WPScriptValue::Boolean(bool value) {
    WPScriptValue result;
    result.shape = WPScriptValueShape::Boolean;
    result.boolean_value = value;
    return result;
}

WPScriptValue WPScriptValue::String(std::string value) {
    WPScriptValue result;
    result.shape = WPScriptValueShape::String;
    result.string_value = std::move(value);
    return result;
}

WPScriptValue WPScriptValue::NumberArray(std::vector<double> values) {
    WPScriptValue result;
    result.shape = WPScriptValueShape::NumberArray;
    result.numeric_values = std::move(values);
    return result;
}

WPScriptValue WPScriptValue::VectorString(std::vector<double> values) {
    WPScriptValue result;
    result.shape = WPScriptValueShape::VectorString;
    result.numeric_values = std::move(values);
    return result;
}

bool WPScriptValue::isNumericVector() const noexcept {
    return shape == WPScriptValueShape::NumberArray || shape == WPScriptValueShape::VectorString;
}

WPScriptRuntime::WPScriptRuntime()
    : m_impl(new Opaque()) {
    m_impl->state.runtime = JS_NewRuntime();
    if (!m_impl->state.runtime) {
        LOG_ERROR("failed to create QuickJS runtime");
        return;
    }

    m_impl->state.context = JS_NewContext(m_impl->state.runtime);
    if (!m_impl->state.context) {
        LOG_ERROR("failed to create QuickJS context");
        JS_FreeRuntime(m_impl->state.runtime);
        m_impl->state.runtime = nullptr;
        return;
    }
}

WPScriptRuntime::~WPScriptRuntime() {
    if (!m_impl) {
        return;
    }

    if (m_impl->state.context) {
        JS_FreeContext(m_impl->state.context);
    }
    if (m_impl->state.runtime) {
        JS_FreeRuntime(m_impl->state.runtime);
    }

    delete m_impl;
}

bool WPScriptRuntime::isReady() const noexcept {
    return m_impl && m_impl->state.runtime && m_impl->state.context;
}

std::optional<WPScriptValue> WPScriptRuntime::evaluate(std::string_view script_source,
                                                       const WPScriptValue& current_value,
                                                       const WPScriptEvaluationContext& context) {
    if (!isReady()) {
        return std::nullopt;
    }

    JSContext* js_context = m_impl->state.context;

    JSValue script_props = JS_NewObject(js_context);
    for (const auto& [name, value] : context.script_properties) {
        JS_SetPropertyStr(js_context, script_props, name.c_str(), ScriptValueToJS(js_context, value));
    }

    JSValue engine = JS_NewObject(js_context);
    JSValue canvas_size = JS_NewObject(js_context);
    JS_SetPropertyStr(js_context, canvas_size, "x", JS_NewFloat64(js_context, context.canvas_size[0]));
    JS_SetPropertyStr(js_context, canvas_size, "y", JS_NewFloat64(js_context, context.canvas_size[1]));
    JS_SetPropertyStr(js_context, engine, "canvasSize", canvas_size);

    JSValue global = JS_GetGlobalObject(js_context);
    InstallGlobalValue(js_context, global, "__scriptProps", JS_DupValue(js_context, script_props));
    InstallGlobalValue(js_context, global, "__currentValue", ScriptValueToJS(js_context, current_value));
    InstallGlobalValue(js_context,
                       global,
                       "__propertyName",
                       JS_NewStringLen(js_context, context.property_name.c_str(), context.property_name.size()));
    InstallGlobalValue(js_context,
                       global,
                       "__videoTextureStates",
                       VideoTextureStatesToJS(js_context, context.video_textures));
    InstallGlobalValue(js_context, global, "__mediaState", MediaStateToJS(js_context, context.media_state));
    InstallGlobalValue(
        js_context, global, "__dispatchMediaThumbnail", JS_NewBool(js_context, context.dispatch_media_thumbnail));
    InstallGlobalValue(
        js_context, global, "__dispatchMediaProperties", JS_NewBool(js_context, context.dispatch_media_properties));
    InstallGlobalValue(
        js_context, global, "__dispatchMediaPlayback", JS_NewBool(js_context, context.dispatch_media_playback));
    InstallGlobalValue(js_context, global, "engine", engine);

    const std::string wrapped = BuildWrappedScript(script_source);
    JSValue result = JS_Eval(js_context,
                             wrapped.c_str(),
                             wrapped.size(),
                             "<scene-script>",
                             JS_EVAL_TYPE_GLOBAL);

    if (!JS_IsException(result)) {
        ReadVideoTextureEventsFromJS(js_context, global, context.video_texture_events);
    }

    InstallGlobalValue(js_context, global, "__scriptProps", JS_UNDEFINED);
    InstallGlobalValue(js_context, global, "__currentValue", JS_UNDEFINED);
    InstallGlobalValue(js_context, global, "__propertyName", JS_UNDEFINED);
    InstallGlobalValue(js_context, global, "__videoTextureStates", JS_UNDEFINED);
    InstallGlobalValue(js_context, global, "__videoTextureEvents", JS_UNDEFINED);
    InstallGlobalValue(js_context, global, "__mediaState", JS_UNDEFINED);
    InstallGlobalValue(js_context, global, "__dispatchMediaThumbnail", JS_UNDEFINED);
    InstallGlobalValue(js_context, global, "__dispatchMediaProperties", JS_UNDEFINED);
    InstallGlobalValue(js_context, global, "__dispatchMediaPlayback", JS_UNDEFINED);
    InstallGlobalValue(js_context, global, "engine", JS_UNDEFINED);
    JS_FreeValue(js_context, global);
    JS_FreeValue(js_context, script_props);

    if (JS_IsException(result)) {
        LogQuickJSException(js_context, "evaluation");
        JS_FreeValue(js_context, result);
        return std::nullopt;
    }

    auto converted = ScriptValueFromJS(js_context, result, current_value);
    JS_FreeValue(js_context, result);
    if (!converted.has_value()) {
        LOG_ERROR("failed to convert QuickJS result back to native script value");
        return std::nullopt;
    }

    return converted;
}

} // namespace wallpaper
