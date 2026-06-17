#pragma once

#include "BackendCapabilities.hpp"
#include "BackendReadyState.hpp"
#include "Diagnostics.hpp"
#include "FrameLifecycle.hpp"
#include "InputEvent.hpp"
#include "OutputSource.hpp"
#include "PropertyValue.hpp"
#include "Result.hpp"
#include "WallpaperTypes.hpp"

#include <string_view>

namespace wallpaper
{
class ContentBackend {
public:
    virtual ~ContentBackend() = default;

    virtual BackendType type() const = 0;
    virtual BackendCapabilities capabilities() const = 0;

    virtual Result<void> load(const WallpaperSource&) = 0;
    virtual Result<void> start() = 0;
    virtual Result<void> pause() = 0;
    virtual Result<void> resume() = 0;
    virtual Result<void> stop() = 0;

    virtual Result<void> setProperty(std::string_view, PropertyValue) = 0;
    virtual Result<void> sendInput(const InputEvent&) = 0;

    virtual Result<void> update() { return Result<void>::success(); }
    virtual Result<bool> produceFrame() { return Result<bool>::success(false); }
    virtual Result<OutputSource*> acquireOutput() {
        return Result<OutputSource*>::success(&outputSource());
    }
    virtual Result<FrameLifecycle> tick() {
        return Result<FrameLifecycle>::success(FrameLifecycle {});
    }
    virtual bool loadsAsynchronously() const { return false; }
    virtual BackendReadyState readyState() const { return BackendReadyState::Loaded; }
    virtual void notifyOutputBound() {}
    virtual OutputSource& outputSource() = 0;
    virtual DiagnosticsSnapshot diagnostics() const = 0;
};
} // namespace wallpaper
