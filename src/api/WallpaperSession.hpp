#pragma once

#include "api/WallpaperTypes.hpp"
#include "common/result/Result.hpp"
#include "output/OutputTarget.hpp"
#include "runtime/diagnostics/Diagnostics.hpp"
#include "runtime/input/InputEvent.hpp"
#include "runtime/property/PropertyValue.hpp"
#include "runtime/session/SessionState.hpp"

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

    SessionState        state() const;
    DiagnosticsSnapshot diagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace wallpaper
