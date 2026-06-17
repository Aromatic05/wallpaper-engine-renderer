#pragma once

#include "Diagnostics.hpp"
#include "FrameLifecycle.hpp"
#include "InputEvent.hpp"
#include "OutputTarget.hpp"
#include "PropertyValue.hpp"
#include "Result.hpp"
#include "SessionState.hpp"
#include "WallpaperTypes.hpp"

#include <memory>
#include <string_view>

namespace wallpaper
{
class WallpaperSession {
public:
    explicit WallpaperSession(SessionConfig config);
    ~WallpaperSession();

    Result<void> load(const WallpaperSource& source);
    Result<void> bindOutput(OutputTarget target);

    Result<void> play();
    Result<void> pause();
    Result<void> stop();
    Result<void> reload();

    Result<void> setProperty(std::string_view name, PropertyValue value);
    Result<void> sendInput(const InputEvent& event);
    Result<FrameLifecycle> tick();

    SessionState        state() const;
    DiagnosticsSnapshot diagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace wallpaper
