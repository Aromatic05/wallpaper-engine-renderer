#include "backend/scene/internal/runtime/WESceneBackend.hpp"

#include <utility>

namespace wallpaper
{
namespace
{
Result<void> unsupportedProperty(std::string_view name) {
    return Result<void>::failure(ResultCode::NotSupported,
                                 "unsupported scene property: " + std::string(name));
}
} // namespace

WESceneOutputSource::WESceneOutputSource(WESceneRuntimeDriver& runtimeDriver)
    : m_runtimeDriver(runtimeDriver) {}

Result<void> WESceneOutputSource::bind(const OutputTarget& target) {
    if (! target.valid()) {
        return Result<void>::failure(ResultCode::InvalidArgument, "output target binding is null");
    }

    std::shared_ptr<WESceneOutputBinding> binding = std::static_pointer_cast<WESceneOutputBinding>(target.binding);
    if (! binding) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires a WE scene output binding");
    }

    if (! m_runtimeDriver.inited()) {
        if (! m_runtimeDriver.init()) {
            return Result<void>::failure(ResultCode::InternalError, "failed to initialize scene wallpaper");
        }
    }

    m_runtimeDriver.initVulkan(binding->renderInitInfo());
    binding->attachSwapchain(m_runtimeDriver.exSwapchain());
    return Result<void>::success();
}

WESceneBackend::WESceneBackend(const BackendContext& context)
    : m_context(context)
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
    // The scene runtime driver routes source/assets through its looper-based command path.
    // Ensure the loopers are initialized before we post load properties, otherwise the
    // early source messages can be dropped and the first frame never arrives.
    if (! m_runtimeDriver.inited()) {
        if (! m_runtimeDriver.init()) {
            return Result<void>::failure(ResultCode::InternalError, "failed to initialize scene wallpaper");
        }
    }

    auto sourceResult = applyProperty(WE_SCENE_PROPERTY_SOURCE, source.uri);
    if (! sourceResult) {
        return sourceResult;
    }

    for (const auto& [name, value] : source.initialProperties) {
        auto propertyResult = applyProperty(name, value);
        if (! propertyResult) {
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

void WESceneBackend::appendDiagnostic(DiagnosticSeverity severity, std::string message) {
    m_diagnostics.append(severity, "backend.scene", std::move(message));
}

} // namespace wallpaper
