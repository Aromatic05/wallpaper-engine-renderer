#include "backend/web/CreateWebBackend.hpp"

#include "backend/web/internal/WebBackend.hpp"
#include "wallpaper/web/WebEngineServices.hpp"

#include "host/HostServices.hpp"

#include <utility>

namespace wallpaper
{
namespace
{
std::shared_ptr<WebEngineServices> CreateDefaultWebEngineServicesImpl() {
    auto services = std::make_shared<WebEngineServices>();
    // Empty paths let CefSettings default-resource resolution run; this is
    // what the upstream WebViewer also relies on (the CEF runtime is staged
    // next to the binary that loaded the runtime library).
    services->provideCefResourcesDir = []() -> std::filesystem::path { return {}; };
    services->provideCefLocalesDir   = []() -> std::filesystem::path { return {}; };
    services->provideCefCacheDir     = []() -> std::filesystem::path { return {}; };
    services->audioMuted             = []() { return true; };
    services->captureAudioSamples    = [](std::chrono::milliseconds)
        -> std::optional<std::array<float, 128>> { return std::nullopt; };
    return services;
}

void mergeMissingWebEngineServices(const std::shared_ptr<WebEngineServices>& target,
                                   const std::shared_ptr<WebEngineServices>& defaults) {
    if (! target || ! defaults) {
        return;
    }
    if (! target->provideCefResourcesDir) target->provideCefResourcesDir = defaults->provideCefResourcesDir;
    if (! target->provideCefLocalesDir)   target->provideCefLocalesDir   = defaults->provideCefLocalesDir;
    if (! target->provideCefCacheDir)     target->provideCefCacheDir     = defaults->provideCefCacheDir;
    if (! target->audioMuted)             target->audioMuted             = defaults->audioMuted;
    if (! target->captureAudioSamples)    target->captureAudioSamples    = defaults->captureAudioSamples;
}

Result<void> validateWebEngineServices(const std::shared_ptr<WebEngineServices>& services) {
    if (! services) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires engine services");
    }
    if (! services->provideCefResourcesDir) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires provideCefResourcesDir");
    }
    if (! services->provideCefLocalesDir) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires provideCefLocalesDir");
    }
    if (! services->provideCefCacheDir) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires provideCefCacheDir");
    }
    if (! services->audioMuted) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires audioMuted");
    }
    if (! services->captureAudioSamples) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires captureAudioSamples");
    }
    return Result<void>::success();
}
} // namespace

std::shared_ptr<WebEngineServices> CreateDefaultWebEngineServices() {
    return CreateDefaultWebEngineServicesImpl();
}

Result<std::unique_ptr<ContentBackend>> CreateWebBackend(const BackendContext&              context,
                                                          std::shared_ptr<WebEngineServices> services) {
    auto validation = validateWebEngineServices(services);
    if (! validation) {
        return Result<std::unique_ptr<ContentBackend>>(validation.error());
    }
    std::unique_ptr<ContentBackend> backend = std::make_unique<WebBackend>(context, std::move(services));
    return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
}
} // namespace wallpaper
