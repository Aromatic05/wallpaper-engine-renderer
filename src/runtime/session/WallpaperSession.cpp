#include "runtime/session/WallpaperSession.hpp"

#include "runtime/backend/ContentBackend.hpp"

namespace wallpaper
{
namespace
{
Result<void> missingFactory() {
    return Result<void>::failure(ResultCode::InvalidState, "session has no backend factory");
}
} // namespace

WallpaperSession::WallpaperSession(SessionConfig config)
    : m_config(std::move(config)) {
    m_backendContext.cachePath = m_config.cachePath;
}

WallpaperSession::~WallpaperSession() = default;

Result<void> WallpaperSession::load(const WallpaperSource& source) {
    if (! m_config.backendFactory) {
        recordError("runtime.session", missingFactory().error());
        m_state = SessionState::Error;
        return missingFactory();
    }

    WallpaperSource resolvedSource = source;
    for (const auto& [name, value] : m_pendingProperties) {
        resolvedSource.initialProperties[name] = value;
    }

    auto backendResult = m_config.backendFactory->create(resolvedSource.type, m_backendContext);
    if (! backendResult) {
        recordError("runtime.session", backendResult.error());
        m_state = SessionState::Error;
        return Result<void>(backendResult.error());
    }

    m_backend = std::move(backendResult.value());
    auto loadResult = m_backend->load(resolvedSource);
    if (! loadResult) {
        recordError("runtime.session", loadResult.error());
        m_state = SessionState::Error;
        return loadResult;
    }

    m_loadedSource = source;
    m_state        = SessionState::Loaded;

    if (m_outputTarget.has_value()) {
        auto bindResult = activateOutputBinding();
        if (! bindResult) {
            m_state = SessionState::Error;
            return bindResult;
        }
    }

    return Result<void>::success();
}

Result<void> WallpaperSession::bindOutput(OutputTarget target) {
    m_outputTarget = std::move(target);
    if (m_backend) {
        return activateOutputBinding();
    }
    return Result<void>::success();
}

Result<void> WallpaperSession::play() {
    auto stateResult = ensureBackend();
    if (! stateResult) {
        return stateResult;
    }

    auto result = m_state == SessionState::Paused ? m_backend->resume() : m_backend->start();
    if (! result) {
        recordError("runtime.session", result.error());
        m_state = SessionState::Error;
        return result;
    }

    m_state = SessionState::Playing;
    return Result<void>::success();
}

Result<void> WallpaperSession::pause() {
    auto stateResult = ensureBackend();
    if (! stateResult) {
        return stateResult;
    }

    auto result = m_backend->pause();
    if (! result) {
        recordError("runtime.session", result.error());
        m_state = SessionState::Error;
        return result;
    }

    m_state = SessionState::Paused;
    return Result<void>::success();
}

Result<void> WallpaperSession::stop() {
    auto stateResult = ensureBackend();
    if (! stateResult) {
        return stateResult;
    }

    auto result = m_backend->stop();
    if (! result) {
        recordError("runtime.session", result.error());
        m_state = SessionState::Error;
        return result;
    }

    m_state = SessionState::Stopped;
    return Result<void>::success();
}

Result<void> WallpaperSession::reload() {
    if (! m_loadedSource.has_value()) {
        return Result<void>::failure(ResultCode::InvalidState, "session has no source to reload");
    }

    auto source = *m_loadedSource;
    m_backend.reset();
    m_state = SessionState::Idle;
    return load(source);
}

Result<void> WallpaperSession::setProperty(std::string_view name, PropertyValue value) {
    std::string propertyName(name);
    m_pendingProperties[propertyName] = value;

    if (! m_backend) {
        return Result<void>::success();
    }

    auto result = m_backend->setProperty(propertyName, std::move(value));
    if (! result) {
        recordError("runtime.property", result.error());
        return result;
    }

    return Result<void>::success();
}

Result<void> WallpaperSession::sendInput(const InputEvent& event) {
    auto stateResult = ensureBackend();
    if (! stateResult) {
        return stateResult;
    }

    auto result = m_backend->sendInput(event);
    if (! result) {
        recordError("runtime.input", result.error());
        return result;
    }

    return Result<void>::success();
}

SessionState WallpaperSession::state() const { return m_state; }

DiagnosticsSnapshot WallpaperSession::diagnostics() const {
    DiagnosticsSnapshot snapshot = m_diagnostics;
    if (m_backend) {
        auto backendDiagnostics = m_backend->diagnostics();
        snapshot.entries.insert(snapshot.entries.end(),
                                backendDiagnostics.entries.begin(),
                                backendDiagnostics.entries.end());
    }
    return snapshot;
}

Result<void> WallpaperSession::ensureBackend() const {
    if (! m_backend) {
        return Result<void>::failure(ResultCode::InvalidState, "session has no loaded backend");
    }
    return Result<void>::success();
}

Result<void> WallpaperSession::activateOutputBinding() {
    if (! m_outputTarget.has_value()) {
        return Result<void>::failure(ResultCode::InvalidState, "session has no output target");
    }
    if (! m_backend) {
        return Result<void>::failure(ResultCode::InvalidState, "session has no backend");
    }

    auto result = m_outputController.bind(*m_outputTarget, m_backend->outputSource());
    if (! result) {
        recordError("runtime.output", result.error());
    }
    return result;
}

void WallpaperSession::recordError(const char* source, const Error& error) {
    appendDiagnostic(DiagnosticSeverity::Error, source, error.message);
}

void WallpaperSession::appendDiagnostic(DiagnosticSeverity severity,
                                        const char*        source,
                                        std::string        message) {
    m_diagnostics.append(severity, source, std::move(message));
}
} // namespace wallpaper
