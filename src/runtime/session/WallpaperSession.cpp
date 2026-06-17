#include "api/WallpaperSession.hpp"

#include "output/OutputController.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/BackendFactory.hpp"
#include "runtime/backend/ContentBackend.hpp"
#include "runtime/diagnostics/DiagnosticsHub.hpp"
#include "runtime/input/InputQueue.hpp"

#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

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

struct WallpaperSession::Impl {
    explicit Impl(SessionConfig sessionConfig)
        : config(std::move(sessionConfig)) {
        backendContext.cachePath = config.cachePath;
    }

    Result<void> ensureBackend() const {
        if (! backend) {
            return Result<void>::failure(ResultCode::InvalidState, "session has no loaded backend");
        }
        return Result<void>::success();
    }

    Result<void> ensureState(
        std::string_view action, std::initializer_list<SessionState> allowedStates) const {
        for (SessionState allowedState : allowedStates) {
            if (state == allowedState) {
                return Result<void>::success();
            }
        }

        return Result<void>::failure(ResultCode::InvalidState,
                                     "cannot " + std::string(action) + " while session is in state "
                                         + sessionStateName(state));
    }

    DiagnosticsSnapshot aggregateDiagnostics() const {
        DiagnosticsHub aggregatedDiagnostics;
        aggregatedDiagnostics.merge(diagnosticsHub.snapshot());
        if (backend) {
            aggregatedDiagnostics.merge(backend->diagnostics());
        }
        return aggregatedDiagnostics.snapshot();
    }

    Result<void> activateOutputBinding() {
        if (! outputTarget.has_value()) {
            return Result<void>::failure(ResultCode::InvalidState, "session has no output target");
        }
        if (! backend) {
            return Result<void>::failure(ResultCode::InvalidState, "session has no backend");
        }

        auto result =
            outputController.bind(*outputTarget, backend->outputSource(), backend->capabilities());
        if (! result) {
            recordError("runtime.output", result.error());
        }
        return result;
    }

    Result<void> drainInputQueue() {
        while (! inputQueue.empty()) {
            auto event = std::move(inputQueue.front());
            inputQueue.pop_front();

            auto result = backend->sendInput(event);
            if (! result) {
                recordError("runtime.input", result.error());
                return result;
            }
        }

        return Result<void>::success();
    }

    Result<void> resetBackendForLoad() {
        if (! backend) {
            inputQueue.clear();
            state = SessionState::Idle;
            return Result<void>::success();
        }

        if (state == SessionState::Loaded || state == SessionState::Playing
            || state == SessionState::Paused) {
            auto stopResult = backend->stop();
            if (! stopResult) {
                recordError("runtime.session", stopResult.error());
                state = SessionState::Error;
                return stopResult;
            }
        }

        backend.reset();
        inputQueue.clear();
        state = SessionState::Idle;
        return Result<void>::success();
    }

    void recordError(const char* source, const Error& error) {
        appendDiagnostic(DiagnosticSeverity::Error, source, error.message);
    }

    void appendDiagnostic(DiagnosticSeverity severity, const char* source, std::string message) {
        diagnosticsHub.append(severity, source, std::move(message));
    }

    SessionConfig                   config;
    BackendContext                  backendContext;
    OutputController                outputController;
    std::unique_ptr<ContentBackend> backend;
    std::optional<WallpaperSource>  loadedSource;
    std::optional<OutputTarget>     outputTarget;
    PropertyMap                     pendingProperties;
    InputQueue                      inputQueue;
    SessionState                    state { SessionState::Idle };
    DiagnosticsHub                  diagnosticsHub;
};

WallpaperSession::WallpaperSession(SessionConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

WallpaperSession::~WallpaperSession() = default;

Result<void> WallpaperSession::load(const WallpaperSource& source) {
    if (! m_impl->config.backendFactory) {
        m_impl->recordError("runtime.session", missingFactory().error());
        m_impl->state = SessionState::Error;
        return missingFactory();
    }

    auto resetResult = m_impl->resetBackendForLoad();
    if (! resetResult) {
        return resetResult;
    }

    WallpaperSource resolvedSource = source;
    for (const auto& [name, value] : m_impl->pendingProperties) {
        resolvedSource.initialProperties[name] = value;
    }

    auto backendResult =
        m_impl->config.backendFactory->create(resolvedSource.type, m_impl->backendContext);
    if (! backendResult) {
        m_impl->recordError("runtime.session", backendResult.error());
        m_impl->state = SessionState::Error;
        return Result<void>(backendResult.error());
    }

    m_impl->backend = std::move(backendResult.value());
    auto loadResult = m_impl->backend->load(resolvedSource);
    if (! loadResult) {
        m_impl->recordError("runtime.session", loadResult.error());
        m_impl->state = SessionState::Error;
        return loadResult;
    }

    m_impl->loadedSource = source;
    m_impl->state        = SessionState::Loaded;

    if (m_impl->outputTarget.has_value()) {
        auto bindResult = m_impl->activateOutputBinding();
        if (! bindResult) {
            m_impl->state = SessionState::Error;
            return bindResult;
        }
    }

    return Result<void>::success();
}

Result<void> WallpaperSession::bindOutput(OutputTarget target) {
    m_impl->outputTarget = std::move(target);
    if (m_impl->backend) {
        return m_impl->activateOutputBinding();
    }
    return Result<void>::success();
}

Result<void> WallpaperSession::play() {
    auto stateResult = m_impl->ensureBackend();
    if (! stateResult) {
        m_impl->recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    stateResult = m_impl->ensureState(
        "play", { SessionState::Loaded, SessionState::Paused, SessionState::Stopped });
    if (! stateResult) {
        m_impl->recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    auto result = m_impl->state == SessionState::Paused ? m_impl->backend->resume()
                                                        : m_impl->backend->start();
    if (! result) {
        m_impl->recordError("runtime.session", result.error());
        m_impl->state = SessionState::Error;
        return result;
    }

    m_impl->state = SessionState::Playing;
    return Result<void>::success();
}

Result<void> WallpaperSession::pause() {
    auto stateResult = m_impl->ensureBackend();
    if (! stateResult) {
        m_impl->recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    stateResult = m_impl->ensureState("pause", { SessionState::Playing });
    if (! stateResult) {
        m_impl->recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    auto result = m_impl->backend->pause();
    if (! result) {
        m_impl->recordError("runtime.session", result.error());
        m_impl->state = SessionState::Error;
        return result;
    }

    m_impl->state = SessionState::Paused;
    return Result<void>::success();
}

Result<void> WallpaperSession::stop() {
    auto stateResult = m_impl->ensureBackend();
    if (! stateResult) {
        m_impl->recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    stateResult = m_impl->ensureState(
        "stop", { SessionState::Loaded, SessionState::Playing, SessionState::Paused });
    if (! stateResult) {
        m_impl->recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    auto result = m_impl->backend->stop();
    if (! result) {
        m_impl->recordError("runtime.session", result.error());
        m_impl->state = SessionState::Error;
        return result;
    }

    m_impl->state = SessionState::Stopped;
    return Result<void>::success();
}

Result<void> WallpaperSession::reload() {
    if (! m_impl->loadedSource.has_value()) {
        auto result =
            Result<void>::failure(ResultCode::InvalidState, "session has no source to reload");
        m_impl->recordError("runtime.session", result.error());
        return result;
    }

    auto source = *m_impl->loadedSource;
    return load(source);
}

Result<void> WallpaperSession::setProperty(std::string_view name, PropertyValue value) {
    std::string propertyName(name);
    m_impl->pendingProperties[propertyName] = value;

    if (! m_impl->backend) {
        return Result<void>::success();
    }

    auto result = m_impl->backend->setProperty(propertyName, std::move(value));
    if (! result) {
        m_impl->recordError("runtime.property", result.error());
        return result;
    }

    return Result<void>::success();
}

Result<void> WallpaperSession::sendInput(const InputEvent& event) {
    auto stateResult = m_impl->ensureBackend();
    if (! stateResult) {
        m_impl->recordError("runtime.input", stateResult.error());
        return stateResult;
    }

    stateResult = m_impl->ensureState(
        "send input", { SessionState::Loaded, SessionState::Playing, SessionState::Paused });
    if (! stateResult) {
        m_impl->recordError("runtime.input", stateResult.error());
        return stateResult;
    }

    m_impl->inputQueue.push_back(event);
    return m_impl->drainInputQueue();
}

SessionState WallpaperSession::state() const { return m_impl->state; }

DiagnosticsSnapshot WallpaperSession::diagnostics() const { return m_impl->aggregateDiagnostics(); }
} // namespace wallpaper
