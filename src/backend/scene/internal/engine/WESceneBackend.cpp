#include "backend/scene/internal/engine/WESceneBackend.hpp"

#include "api/scene/WESceneOutput.hpp"

#include <utility>

namespace wallpaper
{
namespace
{
class LegacyWEScenePresentationPlan final : public RenderPlan {
public:
    explicit LegacyWEScenePresentationPlan(WESceneRuntimeDriver& runtimeDriver)
        : m_runtimeDriver(runtimeDriver) {}

    OutputTargetBindingKind requiredBindingKind() const override {
        return OutputTargetBindingKind::VulkanRenderTarget;
    }

    std::uint64_t revision() const override { return 1; }

    Result<void> bindOutput(const OutputTarget& target) override {
        auto binding = std::dynamic_pointer_cast<WESceneOutputBinding>(target.binding);
        if (! binding) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "scene render plan requires a WE scene output binding");
        }

        if (! m_runtimeDriver.inited()) {
            if (! m_runtimeDriver.init()) {
                return Result<void>::failure(ResultCode::InternalError,
                                             "failed to initialize scene wallpaper");
            }
        }

        m_runtimeDriver.initVulkan(binding->renderInitInfo());
        binding->attachSwapchain(m_runtimeDriver.exSwapchain());
        return Result<void>::success();
    }

private:
    WESceneRuntimeDriver& m_runtimeDriver;
};

Result<void> unsupportedProperty(std::string_view name) {
    return Result<void>::failure(ResultCode::NotSupported,
                                 "unsupported scene property: " + std::string(name));
}
} // namespace

WESceneOutputSource::WESceneOutputSource(WESceneRuntimeDriver& runtimeDriver)
    : m_runtimeDriver(runtimeDriver)
    , m_renderPlan(std::make_shared<LegacyWEScenePresentationPlan>(runtimeDriver)) {}

Result<RenderPlanPtr> WESceneOutputSource::currentRenderPlan() const {
    return Result<RenderPlanPtr>::success(m_renderPlan);
}

WESceneBackend::WESceneBackend(const BackendContext& context,
                               std::shared_ptr<WESceneEngineServices> engineServices)
    : m_context(context)
    , m_sharedState(std::make_shared<SharedState>())
    , m_runtimeDriver(m_context.hostServices, engineServices)
    , m_engineServices(std::move(engineServices))
    , m_outputSource(m_runtimeDriver) {
    if (! m_context.cachePath.empty()) {
        m_runtimeDriver.setPropertyString(WE_SCENE_PROPERTY_CACHE_PATH, m_context.cachePath);
    }
}

BackendType WESceneBackend::type() const { return BackendType::WEScene; }

BackendCapabilities WESceneBackend::capabilities() const {
    BackendCapabilities capabilities;
    capabilities.supportsProperties    = true;
    capabilities.supportsInput         = true;
    capabilities.supportsRenderPlan    = true;
    capabilities.supportsTextureOutput = false;
    capabilities.supportsSurfaceOutput = false;
    return capabilities;
}

Result<void> WESceneBackend::load(const WallpaperSource& source) {
    m_sharedState->readyState.store(BackendReadyState::Loading);
    m_sharedState->outputBound.store(false);
    m_sharedState->contentStateChanged.store(true);
    m_sharedState->outputStateChanged.store(false);
    m_sharedState->frameRequested.store(false);

    if (! m_context.cachePath.empty() && m_context.hostServices
        && m_context.hostServices->fileSystem.createDirectories) {
        const bool cacheReady =
            m_context.hostServices->fileSystem.createDirectories(std::filesystem::path(m_context.cachePath));
        if (! cacheReady) {
            appendDiagnostic(DiagnosticSeverity::Warning,
                             "failed to prepare cache directory before loading scene");
        }
    }

    // The scene runtime driver routes source/assets through its looper-based command path.
    // Ensure the loopers are initialized before we post load properties, otherwise the
    // early source messages can be dropped and the first frame never arrives.
    if (! m_runtimeDriver.inited()) {
        if (! m_runtimeDriver.init()) {
            return Result<void>::failure(ResultCode::InternalError, "failed to initialize scene wallpaper");
        }
    }

    installFirstFrameCallback();

    auto sourceResult = applyProperty(WE_SCENE_PROPERTY_SOURCE, source.uri);
    if (! sourceResult) {
        m_sharedState->readyState.store(BackendReadyState::Error);
        return sourceResult;
    }

    for (const auto& [name, value] : source.initialProperties) {
        auto propertyResult = applyProperty(name, value);
        if (! propertyResult) {
            m_sharedState->readyState.store(BackendReadyState::Error);
            return propertyResult;
        }
    }

    return Result<void>::success();
}

Result<void> WESceneBackend::start() {
    m_runtimeDriver.play();
    return Result<void>::success();
}

Result<void> WESceneBackend::pause() {
    m_runtimeDriver.pause();
    return Result<void>::success();
}

Result<void> WESceneBackend::resume() {
    m_runtimeDriver.play();
    return Result<void>::success();
}

Result<void> WESceneBackend::stop() {
    m_runtimeDriver.pause();
    return Result<void>::success();
}

Result<void> WESceneBackend::setProperty(std::string_view name, PropertyValue value) {
    return applyProperty(name, value);
}

Result<void> WESceneBackend::sendInput(const InputEvent& event) {
    switch (event.type) {
    case InputEventType::PointerMove:
    case InputEventType::PointerDown:
    case InputEventType::PointerUp:
        m_runtimeDriver.mouseInput(event.pointerX, event.pointerY);
        return Result<void>::success();
    case InputEventType::KeyDown:
    case InputEventType::KeyUp:
    case InputEventType::Custom:
        return Result<void>::failure(ResultCode::NotSupported,
                                     "scene backend currently supports pointer input only");
    }

    return Result<void>::failure(ResultCode::NotSupported, "unknown input event type");
}

Result<void> WESceneBackend::update() {
    return Result<void>::success();
}

Result<bool> WESceneBackend::produceFrame() {
    const bool requested = m_sharedState->frameRequested.exchange(false);
    return Result<bool>::success(requested);
}

Result<OutputSource*> WESceneBackend::acquireOutput() {
    return Result<OutputSource*>::success(&m_outputSource);
}

Result<FrameLifecycle> WESceneBackend::tick() {
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

bool WESceneBackend::loadsAsynchronously() const { return true; }

BackendReadyState WESceneBackend::readyState() const { return m_sharedState->readyState.load(); }

void WESceneBackend::notifyOutputBound() {
    m_sharedState->outputBound.store(true);
    m_sharedState->outputStateChanged.store(true);
    if (m_sharedState->readyState.load() == BackendReadyState::Loaded) {
        m_sharedState->readyState.store(BackendReadyState::OutputReady);
        m_sharedState->contentStateChanged.store(true);
    }
}

OutputSource& WESceneBackend::outputSource() { return m_outputSource; }

DiagnosticsSnapshot WESceneBackend::diagnostics() const { return m_diagnostics; }

Result<void> WESceneBackend::applyProperty(std::string_view name, const PropertyValue& value) {
    if (const auto* stringValue = std::get_if<std::string>(&value)) {
        m_runtimeDriver.setPropertyString(name, *stringValue);
        return Result<void>::success();
    }
    if (const auto* boolValue = std::get_if<bool>(&value)) {
        m_runtimeDriver.setPropertyBool(name, *boolValue);
        return Result<void>::success();
    }
    if (const auto* intValue = std::get_if<std::int32_t>(&value)) {
        m_runtimeDriver.setPropertyInt32(name, *intValue);
        return Result<void>::success();
    }
    if (const auto* floatValue = std::get_if<float>(&value)) {
        m_runtimeDriver.setPropertyFloat(name, *floatValue);
        return Result<void>::success();
    }
    if (const auto* doubleValue = std::get_if<double>(&value)) {
        m_runtimeDriver.setPropertyFloat(name, static_cast<float>(*doubleValue));
        return Result<void>::success();
    }
    if (const auto* objectValue = std::get_if<PropertyObject>(&value)) {
        m_runtimeDriver.setPropertyObject(name, *objectValue);
        return Result<void>::success();
    }

    if (name == WE_SCENE_PROPERTY_SOURCE || name == WE_SCENE_PROPERTY_ASSETS
        || name == WE_SCENE_PROPERTY_CACHE_PATH) {
        return unsupportedProperty(name);
    }

    return Result<void>::failure(ResultCode::NotSupported,
                                 "property value type is not supported by scene backend");
}

void WESceneBackend::installFirstFrameCallback() {
    auto weakState = std::weak_ptr<SharedState>(m_sharedState);
    auto callback  = std::make_shared<FirstFrameCallback>([weakState]() {
        if (auto state = weakState.lock()) {
            const auto nextState =
                state->outputBound.load() ? BackendReadyState::OutputReady : BackendReadyState::Loaded;
            state->readyState.store(nextState);
            state->contentStateChanged.store(true);
            state->outputStateChanged.store(state->outputBound.load());
            state->frameRequested.store(true);
        }
    });

    m_runtimeDriver.setPropertyObject(WE_SCENE_PROPERTY_FIRST_FRAME_CALLBACK, callback);
}

void WESceneBackend::appendDiagnostic(DiagnosticSeverity severity, std::string message) {
    m_diagnostics.append(severity, "backend.scene", std::move(message));
}

} // namespace wallpaper
