#pragma once

#include "backend/scene/legacy/SceneWallpaper.hpp"
#include "backend/scene/compatibility/WESceneOutputTarget.hpp"
#include "common/result/Result.hpp"
#include "output/RenderPlanSource.hpp"
#include "output/OutputTarget.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/BackendFactory.hpp"
#include "runtime/backend/ContentBackend.hpp"

#include <memory>

namespace wallpaper
{
class WESceneOutputSource final : public RenderPlanSource {
public:
    explicit WESceneOutputSource(SceneWallpaper& wallpaper);

    Result<void>     bind(const OutputTarget& target) override;

private:
    SceneWallpaper& m_wallpaper;
    bool            m_initialized { false };
};

class WESceneBackend final : public ContentBackend {
public:
    explicit WESceneBackend(const BackendContext& context);

    BackendType         type() const override;
    BackendCapabilities capabilities() const override;

    Result<void> load(const WallpaperSource& source) override;
    Result<void> start() override;
    Result<void> pause() override;
    Result<void> resume() override;
    Result<void> stop() override;

    Result<void> setProperty(std::string_view name, PropertyValue value) override;
    Result<void> sendInput(const InputEvent& event) override;

    OutputSource&        outputSource() override;
    DiagnosticsSnapshot  diagnostics() const override;

private:
    Result<void> applyProperty(std::string_view name, const PropertyValue& value);
    void         appendDiagnostic(DiagnosticSeverity severity, std::string message);

private:
    BackendContext       m_context;
    SceneWallpaper       m_wallpaper;
    WESceneOutputSource  m_outputSource;
    DiagnosticsSnapshot  m_diagnostics;
};

class WESceneBackendFactory final : public BackendFactory {
public:
    Result<std::unique_ptr<ContentBackend>> create(BackendType         type,
                                                   const BackendContext& context) override;
};
} // namespace wallpaper
