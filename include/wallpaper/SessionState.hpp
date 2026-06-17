#pragma once

namespace wallpaper
{
enum class SessionState
{
    Idle,
    Loading,
    Loaded,
    OutputReady,
    Playing,
    Paused,
    Stopped,
    Error
};
} // namespace wallpaper
