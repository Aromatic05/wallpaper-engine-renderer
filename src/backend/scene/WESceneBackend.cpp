#include "backend/scene/WESceneBackend.hpp"

#include "SceneWallpaper.hpp"

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

OutputTarget MakeWESceneOutputTarget(const RenderInitInfo& info) {
    auto payload = std::make_shared<RenderInitInfo>(info);
    OutputTarget target;
    target.type    = info.offscreen ? OutputTargetType::Offscreen : OutputTargetType::Surface;
    target.binding = std::static_pointer_cast<void>(payload);
    target.width   = info.width;
    target.height  = info.height;
    return target;
}

WESceneOutputSource::WESceneOutputSource(SceneWallpaper& wallpaper)
    : m_wallpaper(wallpaper) {}

Result<void> WESceneOutputSource::bind(const OutputTarget& target) {
    if (! target.valid()) {
        return Result<void>::failure(ResultCode::InvalidArgument, "output target binding is null");
    }

    auto initInfo = std::static_pointer_cast<RenderInitInfo>(target.binding);
    if (! initInfo) {
        return Result<void>::failure(ResultCode::InvalidArgument, "scene backend requires render init info");
    }

    if (! m_initialized) {
        if (! m_wallpaper.init()) {
            return Result<void>::failure(ResultCode::InternalError, "failed to initialize scene wallpaper");
        }
        m_initialized = true;
    }

    m_wallpaper.initVulkan(*initInfo);
    return Result<void>::success();
}

WESceneBackend::WESceneBackend(const BackendContext& context)
    : m_context(context)
    , m_outputSource(m_wallpaper) {
    if (! m_context.cachePath.empty()) {
        m_wallpaper.setPropertyString(PROPERTY_CACHE_PATH, m_context.cachePath);
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
    auto sourceResult = applyProperty(PROPERTY_SOURCE, source.uri);
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
    m_wallpaper.play();
    return Result<void>::success();
}

Result<void> WESceneBackend::pause() {
    m_wallpaper.pause();
    return Result<void>::success();
}

Result<void> WESceneBackend::resume() {
    m_wallpaper.play();
    return Result<void>::success();
}

Result<void> WESceneBackend::stop() {
    m_wallpaper.pause();
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
        m_wallpaper.mouseInput(event.pointerX, event.pointerY);
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
        m_wallpaper.setPropertyString(name, *stringValue);
        return Result<void>::success();
    }
    if (const auto* boolValue = std::get_if<bool>(&value)) {
        m_wallpaper.setPropertyBool(name, *boolValue);
        return Result<void>::success();
    }
    if (const auto* intValue = std::get_if<std::int32_t>(&value)) {
        m_wallpaper.setPropertyInt32(name, *intValue);
        return Result<void>::success();
    }
    if (const auto* floatValue = std::get_if<float>(&value)) {
        m_wallpaper.setPropertyFloat(name, *floatValue);
        return Result<void>::success();
    }
    if (const auto* doubleValue = std::get_if<double>(&value)) {
        m_wallpaper.setPropertyFloat(name, static_cast<float>(*doubleValue));
        return Result<void>::success();
    }

    if (name == PROPERTY_SOURCE || name == PROPERTY_ASSETS || name == PROPERTY_CACHE_PATH) {
        return unsupportedProperty(name);
    }

    return Result<void>::failure(ResultCode::NotSupported,
                                 "property value type is not supported by scene backend");
}

void WESceneBackend::appendDiagnostic(DiagnosticSeverity severity, std::string message) {
    m_diagnostics.append(severity, "backend.scene", std::move(message));
}

Result<std::unique_ptr<ContentBackend>> WESceneBackendFactory::create(BackendType          type,
                                                                      const BackendContext& context) {
    if (type != BackendType::WEScene) {
        return Result<std::unique_ptr<ContentBackend>>::failure(
            ResultCode::NotSupported, "factory supports only WEScene backend");
    }

    std::unique_ptr<ContentBackend> backend = std::make_unique<WESceneBackend>(context);
    return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
}
} // namespace wallpaper
