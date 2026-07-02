#pragma once

#include "common/result/Result.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/BackendReadyState.hpp"
#include "runtime/backend/ContentBackend.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;
typedef struct _GstSample GstSample;

namespace wallpaper
{
class VideoOutputBinding;
class VideoOutputSource;
class VideoFrameSwapchain;

class VideoBackend final : public ContentBackend {
public:
    explicit VideoBackend(const BackendContext& context);
    ~VideoBackend() override;

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
    Result<OutputSource*>  acquireOutput() override;
    Result<FrameLifecycle> tick() override;
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

    enum class PipelineMode {
        Dmabuf,
        Shm,
    };

    void         appendDiagnostic(DiagnosticSeverity severity, std::string message);
    Result<void> ensurePipeline();
    void         destroyPipeline();
    Result<void> buildPipeline(PipelineMode mode);
    Result<void> applyPlaybackState();
    Result<void> publishSample(GstSample* sample);
    void         drainBus();

private:
    BackendContext                     m_context;
    std::shared_ptr<SharedState>       m_sharedState;
    std::unique_ptr<VideoOutputSource> m_outputSource;
    std::shared_ptr<VideoOutputBinding> m_renderBinding;
    std::unique_ptr<VideoFrameSwapchain> m_frameSwapchain;
    DiagnosticsSnapshot                m_diagnostics;
    std::filesystem::path              m_sourcePath;
    GstElement*                        m_pipeline { nullptr };
    GstElement*                        m_appsink { nullptr };
    GstBus*                            m_bus { nullptr };
    PipelineMode                       m_preferredPipelineMode { PipelineMode::Dmabuf };
    PipelineMode                       m_pipelineMode { PipelineMode::Dmabuf };
    std::optional<std::string>         m_selectedDmabufDrmFormat;
    std::optional<std::string>         m_lastLoggedDmabufSampleSignature;
    bool                               m_paused { false };
    bool                               m_muted { false };
    bool                               m_started { false };
    bool                               m_eos { false };
    float                              m_volume { 1.0f };
    float                              m_speed { 1.0f };
};
} // namespace wallpaper
