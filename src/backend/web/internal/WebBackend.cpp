#include "backend/web/internal/WebBackend.hpp"

namespace wallpaper
{
WebBackend::WebBackend(const BackendContext& context)
    : m_context(context) {}

BackendType WebBackend::type() const { return BackendType::Web; }

BackendCapabilities WebBackend::capabilities() const {
    BackendCapabilities capabilities;
    capabilities.supportsProperties    = false;
    capabilities.supportsInput         = false;
    capabilities.supportsRenderPlan    = false;
    capabilities.supportsTextureOutput = false;
    capabilities.supportsSurfaceOutput = false;
    return capabilities;
}

Result<void> WebBackend::load(const WallpaperSource& source) {
    m_uri    = source.uri;
    m_loaded = ! m_uri.empty();
    if (! m_loaded) {
        appendDiagnostic(DiagnosticSeverity::Error, "web backend requires a source uri");
        return Result<void>::failure(ResultCode::InvalidArgument, "web backend requires a source uri");
    }

    appendDiagnostic(DiagnosticSeverity::Info,
                     "web backend skeleton loaded source and is ready for future surface output work");
    return Result<void>::success();
}

Result<void> WebBackend::start() {
    if (! m_loaded) {
        return Result<void>::failure(ResultCode::InvalidState, "web backend has no loaded source");
    }

    appendDiagnostic(DiagnosticSeverity::Error,
                     "web backend cannot start because no real web runtime is implemented yet");
    return Result<void>::failure(ResultCode::NotSupported,
                                 "web backend runtime is not implemented yet");
}

Result<void> WebBackend::pause() {
    if (! m_loaded) {
        return Result<void>::failure(ResultCode::InvalidState, "web backend has no loaded source");
    }

    m_started = false;
    return Result<void>::success();
}

Result<void> WebBackend::resume() { return start(); }

Result<void> WebBackend::stop() {
    if (! m_loaded) {
        return Result<void>::failure(ResultCode::InvalidState, "web backend has no loaded source");
    }

    m_started = false;
    return Result<void>::success();
}

Result<void> WebBackend::setProperty(std::string_view name, PropertyValue) {
    appendDiagnostic(DiagnosticSeverity::Warning,
                     "web backend ignored unsupported property: " + std::string(name));
    return Result<void>::failure(ResultCode::NotSupported,
                                 "web backend properties are not implemented yet");
}

Result<void> WebBackend::sendInput(const InputEvent&) {
    appendDiagnostic(DiagnosticSeverity::Warning,
                     "web backend ignored input because input routing is not implemented yet");
    return Result<void>::failure(ResultCode::NotSupported,
                                 "web backend input routing is not implemented yet");
}

Result<FrameLifecycle> WebBackend::tick() {
    return Result<FrameLifecycle>::success(FrameLifecycle {});
}

BackendReadyState WebBackend::readyState() const {
    if (! m_loaded) {
        return BackendReadyState::Idle;
    }
    return BackendReadyState::Loaded;
}

OutputSource& WebBackend::outputSource() { return m_outputSource; }

DiagnosticsSnapshot WebBackend::diagnostics() const { return m_diagnostics; }

void WebBackend::appendDiagnostic(DiagnosticSeverity severity, std::string message) {
    m_diagnostics.append(severity, "backend.web", std::move(message));
}
} // namespace wallpaper
