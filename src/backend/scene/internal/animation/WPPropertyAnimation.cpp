#include "WPPropertyAnimation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <nlohmann/json.hpp>

namespace wallpaper
{
namespace
{

double CubicBezier1D(double p0, double p1, double p2, double p3, double t) {
    const double u  = 1.0 - t;
    const double tt = t * t;
    const double uu = u * u;
    return (uu * u * p0) + (3.0 * uu * t * p1) + (3.0 * u * tt * p2) + (tt * t * p3);
}

bool HasBezierHandles(const WPPropertyAnimationKeyframe& lhs, const WPPropertyAnimationKeyframe& rhs) {
    return lhs.front.enabled || rhs.back.enabled;
}

WPPropertyAnimationKeyframe::Handle ResolveBezierHandle(const WPPropertyAnimationKeyframe::Handle& handle,
                                                        double                                     segment_dx,
                                                        double                                     fps,
                                                        bool                                       forward_handle) {
    if (!handle.enabled) return {};

    auto resolved = handle;
    if (!handle.magic) return resolved;

    const double sign = forward_handle ? 1.0 : -1.0;
    const double slope = std::abs(handle.x) > std::numeric_limits<double>::epsilon()
                             ? (handle.y / handle.x)
                             : 0.0;
    const double auto_dx = std::max(segment_dx / 3.0, 0.0);
    resolved.x = sign * auto_dx;
    // Wallpaper Engine stores magic handle slopes as value-per-second, while animation keyframe
    // positions and Bezier X control points stay in frame units. Convert the generated X distance
    // back to seconds only for the Y offset; otherwise alpha and other scalar curves get their
    // handle height multiplied by fps and can overshoot by tens of times the authored value range.
    resolved.y = slope * (resolved.x / std::max(fps, std::numeric_limits<double>::epsilon()));
    return resolved;
}

double EvaluateBezierSegment(const WPPropertyAnimationKeyframe& lhs,
                             const WPPropertyAnimationKeyframe& rhs,
                             double                             frame,
                             double                             fps) {
    const double segment_dx = std::max(rhs.frame - lhs.frame, 0.0);
    const auto lhs_front = ResolveBezierHandle(lhs.front, segment_dx, fps, true);
    const auto rhs_back  = ResolveBezierHandle(rhs.back, segment_dx, fps, false);
    const double x0 = lhs.frame;
    const double y0 = lhs.value;
    const double x1 = lhs_front.enabled ? lhs.frame + lhs_front.x : lhs.frame;
    const double y1 = lhs_front.enabled ? lhs.value + lhs_front.y : lhs.value;
    const double x2 = rhs_back.enabled ? rhs.frame + rhs_back.x : rhs.frame;
    const double y2 = rhs_back.enabled ? rhs.value + rhs_back.y : rhs.value;
    const double x3 = rhs.frame;
    const double y3 = rhs.value;

    double low = 0.0;
    double high = 1.0;
    for (int iteration = 0; iteration < 24; iteration++) {
        const double mid = (low + high) * 0.5;
        const double x   = CubicBezier1D(x0, x1, x2, x3, mid);
        if (x < frame) {
            low = mid;
        } else {
            high = mid;
        }
    }

    const double t = (low + high) * 0.5;
    return CubicBezier1D(y0, y1, y2, y3, t);
}

bool ParseHandle(const nlohmann::json& json,
                 const char*           key,
                 WPPropertyAnimationKeyframe::Handle& out_handle) {
    out_handle = {};
    if (!json.contains(key) || !json.at(key).is_object()) return false;
    const auto& handle = json.at(key);
    if (handle.contains("enabled") && handle.at("enabled").is_boolean()) {
        out_handle.enabled = handle.at("enabled").get<bool>();
    }
    if (handle.contains("magic") && handle.at("magic").is_boolean()) {
        out_handle.magic = handle.at("magic").get<bool>();
    }
    if (handle.contains("x") && handle.at("x").is_number()) {
        out_handle.x = handle.at("x").get<double>();
    }
    if (handle.contains("y") && handle.at("y").is_number()) {
        out_handle.y = handle.at("y").get<double>();
    }
    return out_handle.enabled;
}

uint32_t ChannelCountForHint(WPDynamicValue::Type hint) {
    switch (hint) {
        case WPDynamicValue::Type::Boolean:
        case WPDynamicValue::Type::Int32:
        case WPDynamicValue::Type::UInt32:
        case WPDynamicValue::Type::Float:
        case WPDynamicValue::Type::Double:
            return 1;
        case WPDynamicValue::Type::Float2:
            return 2;
        case WPDynamicValue::Type::Int3:
        case WPDynamicValue::Type::Float3:
            return 3;
        case WPDynamicValue::Type::Float4:
            return 4;
        case WPDynamicValue::Type::FloatVector:
        case WPDynamicValue::Type::String:
        case WPDynamicValue::Type::Null:
            return 0;
    }
    return 0;
}

double EvaluateKeyframeRange(const WPPropertyAnimationKeyframe& lhs,
                             const WPPropertyAnimationKeyframe& rhs,
                             double                             frame,
                             double                             fps) {
    if (HasBezierHandles(lhs, rhs)) {
        return EvaluateBezierSegment(lhs, rhs, frame, fps);
    }
    const double delta = rhs.frame - lhs.frame;
    if (delta <= std::numeric_limits<double>::epsilon()) return rhs.value;
    const double factor = std::clamp((frame - lhs.frame) / delta, 0.0, 1.0);
    return lhs.value + ((rhs.value - lhs.value) * factor);
}

double EvaluateChannel(const WPPropertyAnimationDefinition& definition,
                       const WPPropertyAnimationChannel&    channel,
                       double                               frame) {
    if (channel.keyframes.empty()) return 0.0;
    if (channel.keyframes.size() == 1) return channel.keyframes.front().value;

    const auto& first = channel.keyframes.front();
    const auto& last  = channel.keyframes.back();

    if (definition.mode == WPPropertyAnimationMode::Loop && definition.wrap_loop) {
        const double frame_count = std::max(definition.frame_count, 0.0);

        if (frame < first.frame) {
            WPPropertyAnimationKeyframe wrapped_last  = last;
            WPPropertyAnimationKeyframe wrapped_first = first;
            wrapped_last.frame -= frame_count;
            return EvaluateKeyframeRange(wrapped_last, wrapped_first, frame, definition.fps);
        }

        if (frame > last.frame) {
            WPPropertyAnimationKeyframe wrapped_first = first;
            wrapped_first.frame += frame_count;
            return EvaluateKeyframeRange(last, wrapped_first, frame, definition.fps);
        }
    }

    if (frame <= first.frame) return first.value;
    if (frame >= last.frame) return last.value;

    for (size_t i = 1; i < channel.keyframes.size(); i++) {
        const auto& lhs = channel.keyframes[i - 1];
        const auto& rhs = channel.keyframes[i];
        if (frame > rhs.frame) continue;
        return EvaluateKeyframeRange(lhs, rhs, frame, definition.fps);
    }

    return last.value;
}

std::array<double, 4> BaseNumericVector(const WPDynamicValue& value, WPDynamicValue::Type hint) {
    std::array<double, 4> result { 0.0, 0.0, 0.0, 0.0 };

    switch (hint) {
        case WPDynamicValue::Type::Boolean: {
            bool scalar = false;
            value.tryGet(&scalar);
            result[0] = scalar ? 1.0 : 0.0;
            break;
        }
        case WPDynamicValue::Type::Int32: {
            int32_t scalar = 0;
            value.tryGet(&scalar);
            result[0] = scalar;
            break;
        }
        case WPDynamicValue::Type::UInt32: {
            uint32_t scalar = 0;
            value.tryGet(&scalar);
            result[0] = scalar;
            break;
        }
        case WPDynamicValue::Type::Float: {
            float scalar = 0.0f;
            value.tryGet(&scalar);
            result[0] = scalar;
            break;
        }
        case WPDynamicValue::Type::Double: {
            double scalar = 0.0;
            value.tryGet(&scalar);
            result[0] = scalar;
            break;
        }
        case WPDynamicValue::Type::Float2: {
            std::array<float, 2> vector {};
            value.tryGet(&vector);
            result[0] = vector[0];
            result[1] = vector[1];
            break;
        }
        case WPDynamicValue::Type::Int3: {
            std::array<int32_t, 3> vector {};
            value.tryGet(&vector);
            result[0] = vector[0];
            result[1] = vector[1];
            result[2] = vector[2];
            break;
        }
        case WPDynamicValue::Type::Float3: {
            std::array<float, 3> vector {};
            value.tryGet(&vector);
            result[0] = vector[0];
            result[1] = vector[1];
            result[2] = vector[2];
            break;
        }
        case WPDynamicValue::Type::Float4: {
            std::array<float, 4> vector {};
            value.tryGet(&vector);
            result[0] = vector[0];
            result[1] = vector[1];
            result[2] = vector[2];
            result[3] = vector[3];
            break;
        }
        case WPDynamicValue::Type::FloatVector:
        case WPDynamicValue::Type::String:
        case WPDynamicValue::Type::Null:
            break;
    }

    return result;
}

double NormalizeFrame(const WPPropertyAnimationDefinition& definition, double frame) {
    const double frame_count = std::max(definition.frame_count, 0.0);
    if (frame_count <= std::numeric_limits<double>::epsilon()) return 0.0;

    switch (definition.mode) {
        case WPPropertyAnimationMode::Loop: {
            double wrapped = std::fmod(frame, frame_count);
            if (wrapped < 0.0) wrapped += frame_count;
            return wrapped;
        }
        case WPPropertyAnimationMode::Mirror: {
            const double period = frame_count * 2.0;
            if (period <= std::numeric_limits<double>::epsilon()) return 0.0;
            double wrapped = std::fmod(frame, period);
            if (wrapped < 0.0) wrapped += period;
            return wrapped <= frame_count ? wrapped : (period - wrapped);
        }
        case WPPropertyAnimationMode::Single:
        default:
            return std::clamp(frame, 0.0, frame_count);
    }
}

bool ShouldStopAtBoundary(const WPPropertyAnimationDefinition& definition, double next_frame) {
    if (definition.mode != WPPropertyAnimationMode::Single) return false;

    const double frame_count = std::max(definition.frame_count, 0.0);
    return next_frame < 0.0 || next_frame > frame_count;
}

} // namespace

bool WPPropertyAnimationDefinition::valid() const noexcept {
    if (channel_count == 0 || frame_count < 0.0 || !std::isfinite(frame_count) || fps <= 0.0 ||
        !std::isfinite(fps)) {
        return false;
    }

    for (uint32_t i = 0; i < channel_count; i++) {
        if (channels[i].keyframes.empty()) return false;
    }

    return true;
}

bool ParsePropertyAnimationDefinition(const nlohmann::json&      json,
                                      WPDynamicValue::Type       hint,
                                      WPPropertyAnimationDefinition& out_definition) {
    out_definition = {};

    if (!json.is_object() || !json.contains("animation") || !json.at("animation").is_object()) {
        return false;
    }

    const auto& animation = json.at("animation");
    const auto expected_channels = ChannelCountForHint(hint);
    if (expected_channels == 0) return false;

    out_definition.channel_count = expected_channels;

    if (animation.contains("options") && animation.at("options").is_object()) {
        const auto& options = animation.at("options");
        if (options.contains("fps") && options.at("fps").is_number()) {
            out_definition.fps = options.at("fps").get<double>();
        }
        if (options.contains("length") && options.at("length").is_number()) {
            out_definition.frame_count = options.at("length").get<double>();
        }
        // Wallpaper Engine stores the user-facing timeline name in animation.options.name.
        // Scene scripts address property timelines through this label, e.g.
        // thisLayer.getAnimation("LogoShake"), so preserving it here lets the script host resolve
        // authored animation names instead of only the property that owns the keyframes.
        if (options.contains("name") && options.at("name").is_string()) {
            out_definition.name = options.at("name").get<std::string>();
        }
        if (options.contains("wraploop") && options.at("wraploop").is_boolean()) {
            out_definition.wrap_loop = options.at("wraploop").get<bool>();
        }
        if (options.contains("startpaused") && options.at("startpaused").is_boolean()) {
            out_definition.start_paused = options.at("startpaused").get<bool>();
        }
        if (options.contains("mode") && options.at("mode").is_string()) {
            const auto mode = options.at("mode").get<std::string>();
            if (mode == "loop") {
                out_definition.mode = WPPropertyAnimationMode::Loop;
            } else if (mode == "mirror") {
                out_definition.mode = WPPropertyAnimationMode::Mirror;
            } else {
                out_definition.mode = WPPropertyAnimationMode::Single;
            }
        }
    }

    if (animation.contains("relative") && animation.at("relative").is_boolean()) {
        out_definition.relative = animation.at("relative").get<bool>();
    }

    for (uint32_t channel_index = 0; channel_index < expected_channels; channel_index++) {
        const std::string channel_name = "c" + std::to_string(channel_index);
        if (!animation.contains(channel_name) || !animation.at(channel_name).is_array()) {
            return false;
        }

        auto& channel = out_definition.channels[channel_index];
        for (const auto& keyframe_json : animation.at(channel_name)) {
            if (!keyframe_json.is_object() || !keyframe_json.contains("frame") ||
                !keyframe_json.contains("value") || !keyframe_json.at("frame").is_number() ||
                !keyframe_json.at("value").is_number()) {
                return false;
            }

            channel.keyframes.push_back(WPPropertyAnimationKeyframe {
                .frame = keyframe_json.at("frame").get<double>(),
                .value = keyframe_json.at("value").get<double>(),
            });
            auto& keyframe = channel.keyframes.back();
            if (keyframe_json.contains("lockangle") && keyframe_json.at("lockangle").is_boolean()) {
                keyframe.lock_angle = keyframe_json.at("lockangle").get<bool>();
            }
            if (keyframe_json.contains("locklength") && keyframe_json.at("locklength").is_boolean()) {
                keyframe.lock_length = keyframe_json.at("locklength").get<bool>();
            }
            ParseHandle(keyframe_json, "front", keyframe.front);
            ParseHandle(keyframe_json, "back", keyframe.back);
        }

        std::sort(channel.keyframes.begin(),
                  channel.keyframes.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.frame < rhs.frame; });
    }

    if (!out_definition.valid()) return false;
    return true;
}

void InitializePropertyAnimationState(const WPPropertyAnimationDefinition& definition,
                                      WPPropertyAnimationState&            state) {
    state.frame   = 0.0;
    state.rate    = 1.0;
    state.playing = !definition.start_paused;
}

bool AdvancePropertyAnimationState(const WPPropertyAnimationDefinition& definition,
                                   WPPropertyAnimationState&            state,
                                   double                               frame_time) {
    if (!state.playing || !definition.valid()) return false;

    const double next_frame = state.frame + (frame_time * definition.fps * state.rate);
    const bool   reached_boundary = ShouldStopAtBoundary(definition, next_frame);

    state.frame = NormalizeFrame(definition, next_frame);
    if (reached_boundary) {
        state.playing = false;
    }

    return reached_boundary;
}

std::optional<WPDynamicValue> EvaluatePropertyAnimation(const WPPropertyAnimationDefinition& definition,
                                                        const WPPropertyAnimationState&      state,
                                                        const WPDynamicValue&                base_value,
                                                        WPDynamicValue::Type                 hint) {
    if (!definition.valid()) return std::nullopt;

    auto result = BaseNumericVector(base_value, hint);
    for (uint32_t i = 0; i < definition.channel_count; i++) {
        const double value = EvaluateChannel(definition, definition.channels[i], state.frame);
        if (definition.relative) {
            result[i] += value;
        } else {
            result[i] = value;
        }
    }

    switch (hint) {
        case WPDynamicValue::Type::Boolean:
            return WPDynamicValue(result[0] >= 0.5);
        case WPDynamicValue::Type::Int32:
            return WPDynamicValue(static_cast<int32_t>(std::lround(result[0])));
        case WPDynamicValue::Type::UInt32:
            return WPDynamicValue(static_cast<uint32_t>(std::max(result[0], 0.0)));
        case WPDynamicValue::Type::Float:
            return WPDynamicValue(static_cast<float>(result[0]));
        case WPDynamicValue::Type::Double:
            return WPDynamicValue(result[0]);
        case WPDynamicValue::Type::Float2:
            return WPDynamicValue(
                std::array<float, 2> { static_cast<float>(result[0]), static_cast<float>(result[1]) });
        case WPDynamicValue::Type::Int3:
            return WPDynamicValue(std::array<int32_t, 3> { static_cast<int32_t>(std::lround(result[0])),
                                                           static_cast<int32_t>(std::lround(result[1])),
                                                           static_cast<int32_t>(std::lround(result[2])) });
        case WPDynamicValue::Type::Float3:
            return WPDynamicValue(std::array<float, 3> { static_cast<float>(result[0]),
                                                         static_cast<float>(result[1]),
                                                         static_cast<float>(result[2]) });
        case WPDynamicValue::Type::Float4:
            return WPDynamicValue(std::array<float, 4> { static_cast<float>(result[0]),
                                                         static_cast<float>(result[1]),
                                                         static_cast<float>(result[2]),
                                                         static_cast<float>(result[3]) });
        case WPDynamicValue::Type::FloatVector:
        case WPDynamicValue::Type::String:
        case WPDynamicValue::Type::Null:
            break;
    }

    return std::nullopt;
}

} // namespace wallpaper
