#pragma once

#include "common/result/Result.hpp"
#include "output/RenderPlanSource.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/BackendReadyState.hpp"
#include "runtime/backend/ContentBackend.hpp"

#include "../../../include/wallpaper/web/WebEngineServices.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace wallpaper
{
class WebBrowserHost;
class WebOutputBinding;
class WebOutputSource;
class WebFrameSwapchain;
struct WebManifestData;

class WebBackend final : public ContentBackend {
public:
    WebBackend(const BackendContext&            context,
               std::shared_ptr<WebEngineServices> services = {});
    ~WebBackend() override;

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
    BackendReadyState      readyState() const override;
    void                   notifyOutputBound() override;
    OutputSource&          outputSource() override;
    DiagnosticsSnapshot    diagnostics() const override;

    // Test-only hook: replace the BrowserHost with a fake. Production
    // code never calls this; the test at commit 11 uses it to assert
    // the contract without spinning up real CEF.
    void testSetBrowserHost(std::shared_ptr<WebBrowserHost> host);

private:
    struct SharedState {
        std::atomic<BackendReadyState> readyState { BackendReadyState::Idle };
        std::atomic<bool>              outputBound { false };
        std::atomic<bool>              contentStateChanged { false };
        std::atomic<bool>              outputStateChanged { false };
        std::atomic<bool>              frameRequested { false };
    };

    void appendDiagnostic(DiagnosticSeverity severity, std::string message);
    bool ensureBrowserHostReady();
    Result<void> validateSubprocessPath(const std::filesystem::path& path);
    std::pair<int, int> resolveInputPixels(const InputEvent& event) const;

private:
    BackendContext                       m_context;
    std::shared_ptr<WebEngineServices>   m_services;
    std::shared_ptr<WebBrowserHost>      m_browserHost;
    std::shared_ptr<WebManifestData>     m_manifest;
    std::filesystem::path                m_workshopDir;
    std::shared_ptr<SharedState>         m_sharedState;
    std::unique_ptr<WebOutputSource>     m_outputSource;
    std::shared_ptr<WebOutputBinding>    m_renderBinding;
    std::unique_ptr<WebFrameSwapchain>   m_frameSwapchain;
    DiagnosticsSnapshot                  m_diagnostics;
};
} // namespace wallpaper
