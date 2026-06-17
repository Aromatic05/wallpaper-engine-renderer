#pragma once

#include "backend/scene/internal/engine/WESceneRuntimeDriver.hpp"
#include "api/scene/WESceneOutput.hpp"
#include "common/result/Result.hpp"
#include "output/OutputTarget.hpp"
#include "output/RenderPlanSource.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/BackendReadyState.hpp"
#include "runtime/backend/ContentBackend.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace wallpaper
{
class WESceneOutputSource final : public RenderPlanSource {
public:
    explicit WESceneOutputSource(WESceneRuntimeDriver& runtimeDriver);

protected:
    Result<RenderPlanPtr> currentRenderPlan() const override;

private:
    WESceneRuntimeDriver& m_runtimeDriver;
    RenderPlanPtr         m_renderPlan;
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

    Result<void>           update() override;
    Result<bool>           produceFrame() override;
    Result<OutputSource*>  acquireOutput() override;
    Result<FrameLifecycle> tick() override;
    bool                   loadsAsynchronously() const override;
    BackendReadyState      readyState() const override;
    void                   notifyOutputBound() override;
    OutputSource&          outputSource() override;
    DiagnosticsSnapshot    diagnostics() const override;

private:
    struct SharedState {
        std::atomic<BackendReadyState> readyState { BackendReadyState::Idle };
        std::atomic<bool>              outputBound { false };
        std::atomic<bool>              contentStateChanged { false };
        std::atomic<bool>              outputStateChanged { false };
        std::atomic<bool>              frameRequested { false };
    };

    Result<void> applyProperty(std::string_view name, const PropertyValue& value);
    void         installFirstFrameCallback();
    void         appendDiagnostic(DiagnosticSeverity severity, std::string message);

private:
    BackendContext               m_context;
    std::shared_ptr<SharedState> m_sharedState;
    WESceneRuntimeDriver         m_runtimeDriver;
    WESceneOutputSource          m_outputSource;
    DiagnosticsSnapshot          m_diagnostics;
};
} // namespace wallpaper
