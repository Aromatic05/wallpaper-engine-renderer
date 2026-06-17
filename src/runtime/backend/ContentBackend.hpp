#pragma once

#include "common/result/Result.hpp"
#include "output/OutputSource.hpp"
#include "runtime/backend/BackendCapabilities.hpp"
#include "runtime/diagnostics/Diagnostics.hpp"
#include "runtime/input/InputEvent.hpp"
#include "runtime/property/PropertyValue.hpp"
#include "runtime/session/WallpaperTypes.hpp"

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

    virtual OutputSource& outputSource() = 0;
    virtual DiagnosticsSnapshot diagnostics() const = 0;
};
} // namespace wallpaper
