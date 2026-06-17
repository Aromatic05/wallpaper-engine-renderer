#include "api/WallpaperSession.hpp"

#include "runtime/backend/ContentBackend.hpp"

namespace wallpaper
{
namespace
{
const char* sessionStateName(SessionState state) {
    switch (state) {
    case SessionState::Idle: return "Idle";
    case SessionState::Loaded: return "Loaded";
    case SessionState::Playing: return "Playing";
    case SessionState::Paused: return "Paused";
    case SessionState::Stopped: return "Stopped";
    case SessionState::Error: return "Error";
    }

    return "Unknown";
}

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

    auto resetResult = resetBackendForLoad();
    if (! resetResult) {
        return resetResult;
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
        recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    stateResult = ensureState("play",
                              { SessionState::Loaded, SessionState::Paused, SessionState::Stopped });
    if (! stateResult) {
        recordError("runtime.session", stateResult.error());
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
        recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    stateResult = ensureState("pause", { SessionState::Playing });
    if (! stateResult) {
        recordError("runtime.session", stateResult.error());
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
        recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    stateResult = ensureState("stop",
                              { SessionState::Loaded, SessionState::Playing, SessionState::Paused });
    if (! stateResult) {
        recordError("runtime.session", stateResult.error());
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
        auto result =
            Result<void>::failure(ResultCode::InvalidState, "session has no source to reload");
        recordError("runtime.session", result.error());
        return result;
    }

    auto source = *m_loadedSource;
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
        recordError("runtime.input", stateResult.error());
        return stateResult;
    }

    stateResult = ensureState("send input",
                              { SessionState::Loaded, SessionState::Playing, SessionState::Paused });
    if (! stateResult) {
        recordError("runtime.input", stateResult.error());
        return stateResult;
    }

    m_inputQueue.push_back(event);
    return drainInputQueue();
}

SessionState WallpaperSession::state() const { return m_state; }

DiagnosticsSnapshot WallpaperSession::diagnostics() const {
    return aggregateDiagnostics();
}

DiagnosticsSnapshot WallpaperSession::aggregateDiagnostics() const {
    DiagnosticsHub aggregatedDiagnostics;
    aggregatedDiagnostics.merge(m_diagnosticsHub.snapshot());
    if (m_backend) {
        aggregatedDiagnostics.merge(m_backend->diagnostics());
    }
    return aggregatedDiagnostics.snapshot();
}

Result<void> WallpaperSession::ensureBackend() const {
    if (! m_backend) {
        return Result<void>::failure(ResultCode::InvalidState, "session has no loaded backend");
    }
    return Result<void>::success();
}

Result<void> WallpaperSession::ensureState(
    std::string_view action, std::initializer_list<SessionState> allowedStates) const {
    for (SessionState state : allowedStates) {
        if (m_state == state) {
            return Result<void>::success();
        }
    }

    return Result<void>::failure(ResultCode::InvalidState,
                                 "cannot " + std::string(action) + " while session is in state "
                                     + sessionStateName(m_state));
}

Result<void> WallpaperSession::activateOutputBinding() {
    if (! m_outputTarget.has_value()) {
        return Result<void>::failure(ResultCode::InvalidState, "session has no output target");
    }
    if (! m_backend) {
        return Result<void>::failure(ResultCode::InvalidState, "session has no backend");
    }

    auto result =
        m_outputController.bind(*m_outputTarget, m_backend->outputSource(), m_backend->capabilities());
    if (! result) {
        recordError("runtime.output", result.error());
    }
    return result;
}

Result<void> WallpaperSession::drainInputQueue() {
    while (! m_inputQueue.empty()) {
        auto event = std::move(m_inputQueue.front());
        m_inputQueue.pop_front();

        auto result = m_backend->sendInput(event);
        if (! result) {
            recordError("runtime.input", result.error());
            return result;
        }
    }

    return Result<void>::success();
}

Result<void> WallpaperSession::resetBackendForLoad() {
    if (! m_backend) {
        m_inputQueue.clear();
        m_state = SessionState::Idle;
        return Result<void>::success();
    }

    if (m_state == SessionState::Loaded || m_state == SessionState::Playing
        || m_state == SessionState::Paused) {
        auto stopResult = m_backend->stop();
        if (! stopResult) {
            recordError("runtime.session", stopResult.error());
            m_state = SessionState::Error;
            return stopResult;
        }
    }

    m_backend.reset();
    m_inputQueue.clear();
    m_state = SessionState::Idle;
    return Result<void>::success();
}

void WallpaperSession::recordError(const char* source, const Error& error) {
    appendDiagnostic(DiagnosticSeverity::Error, source, error.message);
}

void WallpaperSession::appendDiagnostic(DiagnosticSeverity severity,
                                        const char*        source,
                                        std::string        message) {
    m_diagnosticsHub.append(severity, source, std::move(message));
}
} // namespace wallpaper
