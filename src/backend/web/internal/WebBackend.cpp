#include "backend/web/internal/WebBackend.hpp"

#include "backend/web/internal/Manifest.hpp"
#include "backend/web/internal/WebFrameSwapchain.hpp"
#include "backend/web/internal/WebRenderPlan.hpp"

#include "wallpaper/scene/WESceneContract.hpp"
#include "wallpaper/web/WebBrowserHost.hpp"
#include "wallpaper/web/WebOutputBinding.hpp"
#include "wallpaper/web/WebTypes.hpp"

#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <utility>

namespace wallpaper
{
namespace
{
std::filesystem::path WorkshopDirFromSourceUri(const std::string& uri) {
    // The Wallpaper Engine convention for web wallpapers is that
    // `source.uri` is the workshop dir and the entry HTML lives at
    // <workshop>/project.json:file. We do not strip a trailing slash.
    return std::filesystem::path(uri);
}
} // namespace

WebBackend::WebBackend(const BackendContext&              context,
                       std::shared_ptr<WebEngineServices> services)
    : m_context(context)
    , m_services(services ? std::move(services) : CreateDefaultWebEngineServices())
    , m_sharedState(std::make_shared<SharedState>())
{
    // The RenderPlan's bind lambda captures a weak ref to the shared
    // state and a raw ref to the backend. When the OutputController
    // calls bindOutput(target), we stash the WebOutputBinding and
    // advance readiness. The actual CEF init lives in start() so the
    // C ABI can pre-bind before any browser is up.
    auto weakSharedState = std::weak_ptr<SharedState>(m_sharedState);
    auto plan = std::make_shared<WebRenderPlan>([this, weakSharedState](const OutputTarget& target) {
        auto shared = weakSharedState.lock();
        if (! shared) {
            return Result<void>::failure(ResultCode::InvalidState,
                                         "web backend was destroyed before bind");
        }
        auto binding = std::dynamic_pointer_cast<WebOutputBinding>(target.binding);
        if (! binding) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "web render plan requires a WebOutputBinding");
        }
        m_renderBinding = std::move(binding);
        const auto& renderInfo = m_renderBinding->renderInitInfo();
        m_frameSwapchain = std::make_unique<WebFrameSwapchain>(renderInfo.width, renderInfo.height);
        m_renderBinding->attachSwapchain(m_frameSwapchain.get());
        shared->outputBound.store(true);
        shared->outputStateChanged.store(true);
        if (shared->readyState.load() == BackendReadyState::Loaded) {
            shared->readyState.store(BackendReadyState::OutputReady);
            shared->contentStateChanged.store(true);
        }
        return Result<void>::success();
    });
    m_outputSource = std::make_unique<WebOutputSource>(plan);
}

WebBackend::~WebBackend() {
    if (m_browserHost) m_browserHost->Shutdown();
}

BackendType WebBackend::type() const { return BackendType::Web; }

BackendCapabilities WebBackend::capabilities() const {
    BackendCapabilities capabilities;
    capabilities.supportsProperties    = true;
    capabilities.supportsInput         = true;
    capabilities.supportsRenderPlan    = true;
    capabilities.supportsTextureOutput = false;
    capabilities.supportsSurfaceOutput = false;
    return capabilities;
}

Result<void> WebBackend::load(const WallpaperSource& source) {
    m_sharedState->readyState.store(BackendReadyState::Loading);
    m_sharedState->outputBound.store(false);
    m_sharedState->contentStateChanged.store(true);
    m_sharedState->outputStateChanged.store(false);
    m_sharedState->frameRequested.store(false);

    m_workshopDir = WorkshopDirFromSourceUri(source.uri);
    auto manifest = web::LoadWebManifest(m_workshopDir);
    if (! manifest) {
        m_sharedState->readyState.store(BackendReadyState::Error);
        appendDiagnostic(DiagnosticSeverity::Error,
                         "web backend could not parse workshop project.json");
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend: LoadWebManifest returned no manifest");
    }
    m_manifest = std::make_shared<WebManifestData>(std::move(*manifest));

    for (const auto& [name, value] : source.initialProperties) {
        auto propertyResult = setProperty(name, value);
        if (! propertyResult) {
            m_sharedState->readyState.store(BackendReadyState::Error);
            return propertyResult;
        }
    }

    m_sharedState->readyState.store(BackendReadyState::Loaded);
    return Result<void>::success();
}

bool WebBackend::ensureBrowserHostReady() {
    if (m_browserHost) return true;
    m_browserHost = std::make_shared<WebBrowserHost>();
    return m_browserHost != nullptr;
}

Result<void> WebBackend::validateSubprocessPath(const std::filesystem::path& path) {
    if (path.empty()) {
        appendDiagnostic(DiagnosticSeverity::Error,
                         "CEF subprocess helper not found: empty path");
        return Result<void>::failure(ResultCode::NotFound,
                                     "CEF subprocess helper not found");
    }

    std::error_code ec;
    if (! std::filesystem::exists(path, ec) || ec) {
        appendDiagnostic(DiagnosticSeverity::Error,
                         "CEF subprocess helper not found: " + path.string());
        return Result<void>::failure(ResultCode::NotFound,
                                     "CEF subprocess helper not found: " + path.string());
    }

    if (! std::filesystem::is_regular_file(path, ec) || ec) {
        appendDiagnostic(DiagnosticSeverity::Error,
                         "CEF subprocess helper is not a regular file: " + path.string());
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "CEF subprocess helper is not a regular file: " + path.string());
    }

    return Result<void>::success();
}

std::pair<int, int> WebBackend::resolveInputPixels(const InputEvent& event) const {
    int width = 1;
    int height = 1;
    if (m_renderBinding) {
        width = std::max(1, static_cast<int>(m_renderBinding->renderInitInfo().width));
        height = std::max(1, static_cast<int>(m_renderBinding->renderInitInfo().height));
    }

    const auto scale_axis = [](double value, int extent) {
        if (value >= 0.0 && value <= 1.0) {
            return static_cast<int>(std::lround(value * static_cast<double>(extent)));
        }
        return static_cast<int>(std::lround(value));
    };

    int x = scale_axis(event.pointerX, width);
    int y = scale_axis(event.pointerY, height);
    x = std::clamp(x, 0, width > 0 ? width - 1 : 0);
    y = std::clamp(y, 0, height > 0 ? height - 1 : 0);
    return { x, y };
}

Result<void> WebBackend::start() {
    if (! m_manifest) {
        return Result<void>::failure(ResultCode::InvalidState,
                                     "web backend has no loaded manifest");
    }
    if (! ensureBrowserHostReady()) {
        return Result<void>::failure(ResultCode::InternalError,
                                     "web backend BrowserHost allocation failed");
    }

    WebBrowserHost::InitOptions opts {};
    opts.resources_dir         = m_services->provideCefResourcesDir();
    opts.locales_dir           = m_services->provideCefLocalesDir();
    opts.cache_dir             = m_services->provideCefCacheDir();
    opts.browser_subprocess_path = m_services->provideCefSubprocessPath();
    opts.enable_audio          = ! m_services->audioMuted();
    auto helperPathResult = validateSubprocessPath(opts.browser_subprocess_path);
    if (! helperPathResult) {
        return helperPathResult;
    }
    if (! m_browserHost->Init(opts)) {
        return Result<void>::failure(ResultCode::InternalError, "CefInitialize failed");
    }

    int width  = 1280;
    int height = 720;
    if (m_renderBinding) {
        width  = m_renderBinding->renderInitInfo().width;
        height = m_renderBinding->renderInitInfo().height;
    }
    m_browserHost->SetAcceleratedPaintCallback([this](const DmaBufFrame& frame) {
        if (! m_frameSwapchain) return;
        if (! m_frameSwapchain->publishFrame(frame)) {
            appendDiagnostic(DiagnosticSeverity::Warning,
                             "web backend failed to publish accelerated paint frame");
            return;
        }
        m_sharedState->frameRequested.store(true);
    });
    if (! m_browserHost->OpenWallpaper(*m_manifest, m_workshopDir, width, height)) {
        return Result<void>::failure(ResultCode::InternalError, "CEF CreateBrowser failed");
    }

    m_sharedState->readyState.store(BackendReadyState::OutputReady);
    m_sharedState->contentStateChanged.store(true);
    m_sharedState->frameRequested.store(true);
    m_paused = false;
    return Result<void>::success();
}

Result<void> WebBackend::pause() {
    if (m_browserHost) m_browserHost->SetPaused(true);
    m_paused = true;
    return Result<void>::success();
}

Result<void> WebBackend::resume() {
    if (m_browserHost) m_browserHost->SetPaused(false);
    m_paused = false;
    return Result<void>::success();
}

Result<void> WebBackend::stop() {
    if (m_browserHost) {
        m_browserHost->RequestClose();
        m_browserHost->Shutdown();
    }
    m_browserHost.reset();
    if (m_renderBinding) m_renderBinding->attachSwapchain(nullptr);
    m_frameSwapchain.reset();
    m_sharedState->readyState.store(BackendReadyState::Idle);
    m_sharedState->outputBound.store(false);
    m_sharedState->frameRequested.store(false);
    m_paused = false;
    return Result<void>::success();
}

Result<void> WebBackend::setProperty(std::string_view name, PropertyValue value) {
    // Pre-start, the C ABI's we_session_set_source already pushes the
    // WE volume / fps / speed / muted fields into the initial
    // properties map, but those take effect only after start() has
    // wired the BrowserHost. After start, route them through the
    // host.
    if (! m_browserHost) {
        return Result<void>::success();
    }
    if (name == WE_SCENE_PROPERTY_VOLUME) {
        if (const auto* v = std::get_if<float>(&value)) {
            m_browserHost->ApplyVolume(*v);
            return Result<void>::success();
        }
    } else if (name == WE_SCENE_PROPERTY_FPS) {
        if (const auto* v = std::get_if<std::int32_t>(&value)) {
            m_browserHost->SetFrameRate(*v);
            return Result<void>::success();
        }
    } else if (name == WE_SCENE_PROPERTY_MUTED) {
        if (const auto* v = std::get_if<bool>(&value)) {
            m_browserHost->ApplyVolume(*v ? 0.0f : 1.0f);
            return Result<void>::success();
        }
    } else if (name == WE_SCENE_PROPERTY_SPEED) {
        appendDiagnostic(DiagnosticSeverity::Warning,
                         "web backend ignored unsupported property: " + std::string(name));
        return Result<void>::success();
    } else if (name == WE_SCENE_PROPERTY_ASSETS) {
        return Result<void>::failure(ResultCode::NotSupported,
                                     "web backend does not support setting assets");
    } else if (name == WE_SCENE_PROPERTY_SOURCE) {
        return Result<void>::failure(ResultCode::NotSupported,
                                     "web backend does not support source reload");
    }
    return Result<void>::failure(ResultCode::NotSupported,
                                 "web backend received unknown property: " + std::string(name));
}

Result<void> WebBackend::sendInput(const InputEvent& event) {
    if (! m_browserHost) {
        return Result<void>::failure(ResultCode::InvalidState, "web backend has no BrowserHost");
    }
    const auto [px, py] = resolveInputPixels(event);
    switch (event.type) {
    case InputEventType::PointerMove:
        m_browserHost->OnMouseMove(px, py, /*left_down=*/false);
        return Result<void>::success();
    case InputEventType::PointerDown:
        m_browserHost->OnMouseMove(px, py, /*left_down=*/true);
        m_browserHost->OnMouseButton(px, py, /*cef_button=*/0, /*down=*/true, /*click_count=*/1);
        return Result<void>::success();
    case InputEventType::PointerUp:
        m_browserHost->OnMouseMove(px, py, /*left_down=*/false);
        m_browserHost->OnMouseButton(px, py, /*cef_button=*/0, /*down=*/false, /*click_count=*/1);
        return Result<void>::success();
    case InputEventType::KeyDown:
    case InputEventType::KeyUp:
    case InputEventType::Custom:
        return Result<void>::failure(ResultCode::NotSupported,
                                     "web backend currently supports pointer input only");
    }
    return Result<void>::failure(ResultCode::NotSupported, "unknown input event type");
}

Result<void> WebBackend::update() {
    if (m_browserHost) {
        if (! m_paused) {
            m_browserHost->Invalidate();
        }
        m_browserHost->Pump();
    }
    return Result<void>::success();
}

Result<bool> WebBackend::produceFrame() {
    return Result<bool>::success(m_sharedState->frameRequested.exchange(false));
}

Result<OutputSource*> WebBackend::acquireOutput() {
    return Result<OutputSource*>::success(&*m_outputSource);
}

Result<FrameLifecycle> WebBackend::tick() {
    FrameLifecycle lifecycle;
    lifecycle.contentStateChanged = m_sharedState->contentStateChanged.exchange(false);
    lifecycle.outputStateChanged  = m_sharedState->outputStateChanged.exchange(false);
    auto frameResult = produceFrame();
    if (! frameResult) {
        return Result<FrameLifecycle>(frameResult.error());
    }
    lifecycle.frameRequested = frameResult.value();
    return Result<FrameLifecycle>::success(std::move(lifecycle));
}

BackendReadyState WebBackend::readyState() const { return m_sharedState->readyState.load(); }

void WebBackend::notifyOutputBound() {
    m_sharedState->outputBound.store(true);
    m_sharedState->outputStateChanged.store(true);
    if (m_sharedState->readyState.load() == BackendReadyState::Loaded) {
        m_sharedState->readyState.store(BackendReadyState::OutputReady);
        m_sharedState->contentStateChanged.store(true);
    }
}

OutputSource& WebBackend::outputSource() { return *m_outputSource; }

DiagnosticsSnapshot WebBackend::diagnostics() const { return m_diagnostics; }

void WebBackend::testSetBrowserHost(std::shared_ptr<WebBrowserHost> host) {
    m_browserHost = std::move(host);
}

void WebBackend::appendDiagnostic(DiagnosticSeverity severity, std::string message) {
    m_diagnostics.append(severity, "backend.web", std::move(message));
}
} // namespace wallpaper
