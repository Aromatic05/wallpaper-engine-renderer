#pragma once

namespace wallpaper
{
enum class BackendReadyState
{
    Idle,
    Loading,
    Loaded,
    OutputReady,
    Error
};
} // namespace wallpaper
