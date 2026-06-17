#pragma once

#include <cstdint>
#include <string>

namespace wallpaper
{
enum class InputEventType
{
    PointerMove,
    PointerDown,
    PointerUp,
    KeyDown,
    KeyUp,
    Custom
};

struct InputEvent {
    InputEventType type { InputEventType::Custom };
    double         pointerX { 0.0 };
    double         pointerY { 0.0 };
    std::int32_t   button { 0 };
    std::int32_t   keyCode { 0 };
    std::string    payload;
};
} // namespace wallpaper
