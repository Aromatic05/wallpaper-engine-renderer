#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "WPDynamicValue.hpp"

namespace wallpaper
{

struct WPPropertyAnimationKeyframe {
    struct Handle {
        bool   enabled { false };
        bool   magic { false };
        double x { 0.0 };
        double y { 0.0 };
    };

    double frame { 0.0 };
    double value { 0.0 };
    bool   lock_angle { false };
    bool   lock_length { false };
    Handle front;
    Handle back;
};

struct WPPropertyAnimationChannel {
    std::vector<WPPropertyAnimationKeyframe> keyframes;
};

enum class WPPropertyAnimationMode
{
    Single,
    Loop,
    Mirror,
};

struct WPPropertyAnimationDefinition {
    std::array<WPPropertyAnimationChannel, 4> channels;
    uint32_t                                  channel_count { 0 };
    double                                    fps { 30.0 };
    double                                    frame_count { 0.0 };
    WPPropertyAnimationMode                   mode { WPPropertyAnimationMode::Single };
    bool                                      wrap_loop { false };
    bool                                      start_paused { false };
    bool                                      relative { false };
    std::string                               name;

    bool valid() const noexcept;
};

struct WPPropertyAnimationState {
    uint32_t animation_id { 0 };
    double   frame { 0.0 };
    double   rate { 1.0 };
    bool     playing { false };
};

bool ParsePropertyAnimationDefinition(const nlohmann::json&      json,
                                      WPDynamicValue::Type       hint,
                                      WPPropertyAnimationDefinition& out_definition);
void InitializePropertyAnimationState(const WPPropertyAnimationDefinition& definition,
                                      WPPropertyAnimationState&            state);
bool AdvancePropertyAnimationState(const WPPropertyAnimationDefinition& definition,
                                   WPPropertyAnimationState&            state,
                                   double                               frame_time);
std::optional<WPDynamicValue> EvaluatePropertyAnimation(const WPPropertyAnimationDefinition& definition,
                                                        const WPPropertyAnimationState&      state,
                                                        const WPDynamicValue&                base_value,
                                                        WPDynamicValue::Type                 hint);

} // namespace wallpaper
