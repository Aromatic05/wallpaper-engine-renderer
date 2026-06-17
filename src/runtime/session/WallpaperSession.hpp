#pragma once

#include "common/result/Result.hpp"
#include "output/OutputController.hpp"
#include "output/OutputTarget.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/BackendFactory.hpp"
#include "runtime/diagnostics/Diagnostics.hpp"
#include "runtime/input/InputEvent.hpp"
#include "runtime/property/PropertyValue.hpp"
#include "runtime/session/SessionState.hpp"
#include "runtime/session/WallpaperTypes.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace wallpaper
{
class ContentBackend;

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

    SessionState         state() const;
    DiagnosticsSnapshot  diagnostics() const;

private:
    Result<void> ensureBackend() const;
    Result<void> activateOutputBinding();
    void         recordError(const char* source, const Error& error);
    void         appendDiagnostic(DiagnosticSeverity severity, const char* source, std::string message);

private:
    SessionConfig                    m_config;
    BackendContext                   m_backendContext;
    OutputController                 m_outputController;
    std::unique_ptr<ContentBackend>  m_backend;
    std::optional<WallpaperSource>   m_loadedSource;
    std::optional<OutputTarget>      m_outputTarget;
    SessionState                     m_state { SessionState::Idle };
    DiagnosticsSnapshot              m_diagnostics;
};
} // namespace wallpaper
