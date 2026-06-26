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
    PointerWheel,
    KeyDown,
    KeyUp,
    FocusGained,
    FocusLost,
    Custom
};

struct InputEvent {
    InputEventType type { InputEventType::Custom };
    double         pointerX { 0.0 };
    double         pointerY { 0.0 };
    std::int32_t   button { 0 };
    std::int32_t   wheelDeltaX { 0 };
    std::int32_t   wheelDeltaY { 0 };
    std::int32_t   keyCode { 0 };
    std::int32_t   nativeKeyCode { 0 };
    std::int32_t   modifiers { 0 };
    std::uint32_t  unicodeChar { 0 };
    std::string    payload;
};
} // namespace wallpaper
