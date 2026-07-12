#include "api/WallpaperSession.hpp"

#include "output/OutputController.hpp"
#include "host/HostServices.hpp"
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
    case SessionState::Loading: return "Loading";
    case SessionState::Loaded: return "Loaded";
    case SessionState::OutputReady: return "OutputReady";
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

std::string resolveCachePath(const SessionConfig& config) {
    if (! config.cachePath.empty()) {
        return config.cachePath;
    }
    if (! config.hostServices || ! config.hostServices->cache.resolveCacheRoot) {
        return {};
    }
    return config.hostServices->cache.resolveCacheRoot("wallpaper-engine-renderer").string();
}

SessionState sessionStateFromReadyState(BackendReadyState readyState) {
    switch (readyState) {
    case BackendReadyState::Idle: return SessionState::Idle;
    case BackendReadyState::Loading: return SessionState::Loading;
    case BackendReadyState::Loaded: return SessionState::Loaded;
    case BackendReadyState::OutputReady: return SessionState::OutputReady;
    case BackendReadyState::Error: return SessionState::Error;
    }

    return SessionState::Error;
}

SessionState sessionStateFromStates(BackendReadyState readyState, PlaybackState playbackState) {
    if (readyState == BackendReadyState::Error) {
        return SessionState::Error;
    }

    switch (playbackState) {
    case PlaybackState::Playing: return SessionState::Playing;
    case PlaybackState::Paused: return SessionState::Paused;
    case PlaybackState::Stopped: return SessionState::Stopped;
    case PlaybackState::Idle: return sessionStateFromReadyState(readyState);
    }

    return SessionState::Error;
}
} // namespace

struct WallpaperSession::Impl {
    explicit Impl(SessionConfig sessionConfig)
        : config(std::move(sessionConfig)) {
        if (! config.hostServices) {
            config.hostServices = CreateDefaultHostServices();
        }

        backendContext.hostServices = config.hostServices;
        backendContext.cachePath    = resolveCachePath(config);
    }

    Result<void> ensureBackend() const {
        if (! backend) {
            return Result<void>::failure(ResultCode::InvalidState, "session has no loaded backend");
        }
        return Result<void>::success();
    }

    Result<void> ensureState(
        std::string_view action, std::initializer_list<SessionState> allowedStates) const {
        const auto currentState = state();
        for (SessionState allowedState : allowedStates) {
            if (currentState == allowedState) {
                return Result<void>::success();
            }
        }

        return Result<void>::failure(ResultCode::InvalidState,
                                     "cannot " + std::string(action) + " while session is in state "
                                         + sessionStateName(currentState));
    }

    DiagnosticsSnapshot aggregateDiagnostics() const {
        DiagnosticsHub aggregatedDiagnostics;
        aggregatedDiagnostics.merge(diagnosticsHub.snapshot());
        if (backend) {
            aggregatedDiagnostics.merge(backend->diagnostics());
        }
        return aggregatedDiagnostics.snapshot();
    }

    void synchronizeSessionState() {
        if (! backend) {
            readyState = BackendReadyState::Idle;
            return;
        }

        readyState = backend->readyState();
    }

    SessionState state() const { return sessionStateFromStates(readyState, playbackState); }

    void setErrorState() {
        readyState     = BackendReadyState::Error;
        playbackState  = PlaybackState::Idle;
    }

    Result<void> bindOutputTarget(const OutputTarget& candidateTarget,
                                  OutputSource&       outputSource,
                                  OutputController&   candidateController,
                                  const RenderPlanPtr& resolvedPlan = nullptr) {
        if (! candidateTarget.valid()) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "output target binding is null");
        }
        if (! backend) {
            return Result<void>::failure(ResultCode::InvalidState, "session has no backend");
        }

        auto result = resolvedPlan
                          ? candidateController.bind(candidateTarget, resolvedPlan)
                          : candidateController.bind(candidateTarget, outputSource);
        if (! result) {
            return result;
        }

        outputTarget      = candidateTarget;
        boundOutputSource = &outputSource;
        outputController  = std::move(candidateController);
        backend->notifyOutputBound();
        synchronizeSessionState();
        return Result<void>::success();
    }

    Result<void> activateOutputBinding(OutputSource& outputSource,
                                       const RenderPlanPtr& resolvedPlan = nullptr) {
        if (! outputTarget.has_value()) {
            return Result<void>::failure(ResultCode::InvalidState, "session has no output target");
        }

        auto candidateController = outputController;
        auto result = bindOutputTarget(*outputTarget, outputSource, candidateController, resolvedPlan);
        if (! result) {
            recordError("runtime.output", result.error());
        }
        return result;
    }

    Result<bool> refreshOutputBinding(OutputSource& outputSource) {
        if (! outputTarget.has_value()) {
            return Result<bool>::success(false);
        }

        if (boundOutputSource != &outputSource) {
            auto result = activateOutputBinding(outputSource);
            if (! result) {
                return Result<bool>(result.error());
            }
            return Result<bool>::success(true);
        }


        auto planResult = outputSource.renderPlan();
        if (! planResult) {
            recordError("runtime.output", planResult.error());
            return Result<bool>(planResult.error());
        }

        const auto& plan = planResult.value();
        if (! plan) {
            auto result = Result<void>::failure(ResultCode::InvalidState,
                                                "render plan source returned a null plan");
            recordError("runtime.output", result.error());
            return Result<bool>(result.error());
        }

        const bool renderPlanChanged = outputController.boundRenderPlan() != plan.get()
                                       || ! outputController.boundRenderPlanRevision().has_value()
                                       || outputController.boundRenderPlanRevision().value()
                                              != plan->revision();
        if (! renderPlanChanged) {
            return Result<bool>::success(false);
        }

        auto result = activateOutputBinding(outputSource, plan);
        if (! result) {
            return Result<bool>(result.error());
        }
        return Result<bool>::success(true);
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
            readyState    = BackendReadyState::Idle;
            playbackState = PlaybackState::Idle;
            boundOutputSource = nullptr;
            outputController = OutputController {};
            return Result<void>::success();
        }

        if (state() == SessionState::Loading || state() == SessionState::Loaded
            || state() == SessionState::OutputReady || state() == SessionState::Playing
            || state() == SessionState::Paused) {
            auto stopResult = backend->stop();
            if (! stopResult) {
                recordError("runtime.session", stopResult.error());
                setErrorState();
                return stopResult;
            }
        }

        backend.reset();
        inputQueue.clear();
        readyState       = BackendReadyState::Idle;
        playbackState    = PlaybackState::Idle;
        boundOutputSource = nullptr;
        outputController = OutputController {};
        return Result<void>::success();
    }

    void recordError(const char* source, const Error& error) {
        appendDiagnostic(DiagnosticSeverity::Error, source, error.message);
    }

    void appendDiagnostic(DiagnosticSeverity severity, const char* source, std::string message) {
        if (backendContext.hostServices && backendContext.hostServices->diagnostics.publish) {
            backendContext.hostServices->diagnostics.publish(severity, source, message);
        }
        diagnosticsHub.append(severity, source, std::move(message));
    }

    SessionConfig                   config;
    BackendContext                  backendContext;
    OutputController                outputController;
    std::unique_ptr<ContentBackend> backend;
    std::optional<WallpaperSource>  loadedSource;
    std::optional<OutputTarget>     outputTarget;
    OutputSource*                   boundOutputSource { nullptr };
    PropertyMap                     pendingProperties;
    InputQueue                      inputQueue;
    BackendReadyState               readyState { BackendReadyState::Idle };
    PlaybackState                   playbackState { PlaybackState::Idle };
    DiagnosticsHub                  diagnosticsHub;
};

WallpaperSession::WallpaperSession(SessionConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

WallpaperSession::~WallpaperSession() = default;

Result<void> WallpaperSession::load(const WallpaperSource& source) {
    if (! m_impl->config.backendFactory) {
        m_impl->recordError("runtime.session", missingFactory().error());
        m_impl->setErrorState();
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
        m_impl->setErrorState();
        return Result<void>(backendResult.error());
    }

    m_impl->backend = std::move(backendResult.value());
    auto loadResult = m_impl->backend->load(resolvedSource);
    if (! loadResult) {
        m_impl->recordError("runtime.session", loadResult.error());
        m_impl->setErrorState();
        return loadResult;
    }

    m_impl->loadedSource = source;
    m_impl->playbackState = PlaybackState::Idle;
    m_impl->synchronizeSessionState();

    if (m_impl->outputTarget.has_value()) {
        auto bindResult = m_impl->activateOutputBinding(m_impl->backend->outputSource());
        if (! bindResult) {
            m_impl->setErrorState();
            return bindResult;
        }
    }

    return Result<void>::success();
}

Result<void> WallpaperSession::bindOutput(OutputTarget target) {
    if (! target.valid()) {
        return Result<void>::failure(ResultCode::InvalidArgument, "output target binding is null");
    }

    if (! m_impl->backend) {
        m_impl->outputTarget = std::move(target);
        return Result<void>::success();
    }

    auto              candidateTarget      = std::move(target);
    auto              candidateController  = m_impl->outputController;
    auto* const       previousOutputSource = m_impl->boundOutputSource;
    auto              result = m_impl->bindOutputTarget(
        candidateTarget, m_impl->backend->outputSource(), candidateController);
    if (! result) {
        m_impl->boundOutputSource = previousOutputSource;
        m_impl->recordError("runtime.output", result.error());
        return result;
    }

    return Result<void>::success();
}

Result<void> WallpaperSession::play() {
    m_impl->synchronizeSessionState();

    auto stateResult = m_impl->ensureBackend();
    if (! stateResult) {
        m_impl->recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    stateResult = m_impl->ensureState(
        "play",
        { SessionState::Loading, SessionState::Loaded, SessionState::OutputReady, SessionState::Paused,
          SessionState::Stopped });
    if (! stateResult) {
        m_impl->recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    auto result = m_impl->playbackState == PlaybackState::Paused ? m_impl->backend->resume()
                                                                 : m_impl->backend->start();
    if (! result) {
        m_impl->recordError("runtime.session", result.error());
        m_impl->setErrorState();
        return result;
    }

    m_impl->playbackState = PlaybackState::Playing;
    return Result<void>::success();
}

Result<void> WallpaperSession::pause() {
    m_impl->synchronizeSessionState();

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
        m_impl->setErrorState();
        return result;
    }

    m_impl->playbackState = PlaybackState::Paused;
    return Result<void>::success();
}

Result<void> WallpaperSession::stop() {
    m_impl->synchronizeSessionState();

    auto stateResult = m_impl->ensureBackend();
    if (! stateResult) {
        m_impl->recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    stateResult = m_impl->ensureState(
        "stop", { SessionState::Loading, SessionState::Loaded, SessionState::OutputReady,
                  SessionState::Playing, SessionState::Paused });
    if (! stateResult) {
        m_impl->recordError("runtime.session", stateResult.error());
        return stateResult;
    }

    auto result = m_impl->backend->stop();
    if (! result) {
        m_impl->recordError("runtime.session", result.error());
        m_impl->setErrorState();
        return result;
    }

    m_impl->playbackState = PlaybackState::Stopped;
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
    m_impl->synchronizeSessionState();

    auto stateResult = m_impl->ensureBackend();
    if (! stateResult) {
        m_impl->recordError("runtime.input", stateResult.error());
        return stateResult;
    }

    stateResult = m_impl->ensureState(
        "send input", { SessionState::Loading, SessionState::Loaded, SessionState::OutputReady,
                        SessionState::Playing, SessionState::Paused });
    if (! stateResult) {
        m_impl->recordError("runtime.input", stateResult.error());
        return stateResult;
    }

    m_impl->inputQueue.push_back(event);
    return m_impl->drainInputQueue();
}

Result<FrameLifecycle> WallpaperSession::tick() {
    if (! m_impl->backend) {
        return Result<FrameLifecycle>::success(FrameLifecycle {});
    }

    const auto previousState          = m_impl->state();
    const auto diagnosticsCountBefore = m_impl->aggregateDiagnostics().entries.size();
    auto updateResult = m_impl->backend->update();
    if (! updateResult) {
        m_impl->recordError("runtime.update", updateResult.error());
        m_impl->setErrorState();
        return Result<FrameLifecycle>(updateResult.error());
    }

    auto tickResult = m_impl->backend->tick();
    if (! tickResult) {
        m_impl->recordError("runtime.tick", tickResult.error());
        m_impl->setErrorState();
        return Result<FrameLifecycle>(tickResult.error());
    }

    auto outputResult = m_impl->backend->acquireOutput();
    if (! outputResult) {
        m_impl->recordError("runtime.output", outputResult.error());
        m_impl->setErrorState();
        return Result<FrameLifecycle>(outputResult.error());
    }

    if (outputResult.value()) {
        auto rebindResult = m_impl->refreshOutputBinding(*outputResult.value());
        if (! rebindResult) {
            m_impl->setErrorState();
            return Result<FrameLifecycle>(rebindResult.error());
        }
        tickResult.value().outputStateChanged = tickResult.value().outputStateChanged || rebindResult.value();
    }

    auto lifecycle = tickResult.value();
    m_impl->synchronizeSessionState();
    lifecycle.contentStateChanged = lifecycle.contentStateChanged || (previousState != m_impl->state());
    lifecycle.diagnosticsChanged =
        lifecycle.diagnosticsChanged
        || (diagnosticsCountBefore != m_impl->aggregateDiagnostics().entries.size());
    return Result<FrameLifecycle>::success(std::move(lifecycle));
}

BackendReadyState WallpaperSession::readyState() const {
    m_impl->synchronizeSessionState();
    return m_impl->readyState;
}

PlaybackState WallpaperSession::playbackState() const { return m_impl->playbackState; }

SessionState WallpaperSession::state() const {
    m_impl->synchronizeSessionState();
    return m_impl->state();
}

DiagnosticsSnapshot WallpaperSession::diagnostics() const { return m_impl->aggregateDiagnostics(); }
} // namespace wallpaper
