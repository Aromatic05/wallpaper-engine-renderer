#pragma once

#include "common/result/Result.hpp"
#include "output/SurfaceSource.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/ContentBackend.hpp"

namespace wallpaper
{
class WebOutputSource final : public SurfaceSource {
public:
    Result<void> bind(const OutputTarget& target) override;
};

class WebBackend final : public ContentBackend {
public:
    explicit WebBackend(const BackendContext& context);

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
    void appendDiagnostic(DiagnosticSeverity severity, std::string message);

private:
    BackendContext      m_context;
    WebOutputSource     m_outputSource;
    DiagnosticsSnapshot m_diagnostics;
    std::string         m_uri;
    bool                m_loaded { false };
    bool                m_started { false };
};
} // namespace wallpaper
