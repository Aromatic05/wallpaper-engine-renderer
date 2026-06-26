#include "backend/web/CreateWebBackend.hpp"

#include "backend/web/internal/WebBackend.hpp"
#include "wallpaper/web/WebEngineServices.hpp"

#include "host/HostServices.hpp"

#include <cstdlib>
#include <vector>

#include <utility>

namespace wallpaper
{
namespace
{
std::filesystem::path helperPathFromEnv() {
    if (const char* value = std::getenv("WE_CEF_HELPER_PATH")) {
        if (*value) return value;
    }
    return {};
}

std::filesystem::path firstExistingRegularFile(const std::vector<std::filesystem::path>& candidates) {
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        ec.clear();
        if (std::filesystem::exists(candidate, ec) && ! ec &&
            std::filesystem::is_regular_file(candidate, ec) && ! ec) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path resolveDefaultCefHelperPath() {
    if (auto fromEnv = helperPathFromEnv(); ! fromEnv.empty()) {
        return fromEnv;
    }

    std::vector<std::filesystem::path> candidates;
    if (const char* cefRoot = std::getenv("CEF_ROOT")) {
        if (*cefRoot) {
            const std::filesystem::path root { cefRoot };
            candidates.push_back(root / "Release" / "we-cef-helper");
            candidates.push_back(root / "we-cef-helper");
        }
    }

    candidates.push_back(std::filesystem::path("/usr/libexec/wallpaper-engine-renderer/we-cef-helper"));
    candidates.push_back(std::filesystem::path("/usr/local/libexec/wallpaper-engine-renderer/we-cef-helper"));
    candidates.push_back(std::filesystem::current_path() / "we-cef-helper");
    candidates.push_back(std::filesystem::current_path() / "build" / "src" / "backend" / "web" / "we-cef-helper");
    candidates.push_back(std::filesystem::current_path() / "build-check" / "src" / "backend" / "web" / "we-cef-helper");
    return firstExistingRegularFile(candidates);
}

std::shared_ptr<WebEngineServices> CreateDefaultWebEngineServicesImpl() {
    auto services = std::make_shared<WebEngineServices>();
    // Empty paths let CefSettings default-resource resolution run; this is
    // what the upstream WebViewer also relies on (the CEF runtime is staged
    // next to the binary that loaded the runtime library).
    services->provideCefResourcesDir = []() -> std::filesystem::path { return {}; };
    services->provideCefLocalesDir   = []() -> std::filesystem::path { return {}; };
    services->provideCefCacheDir     = []() -> std::filesystem::path { return {}; };
    services->provideCefSubprocessPath = []() -> std::filesystem::path {
        return resolveDefaultCefHelperPath();
    };
    services->audioMuted             = []() { return true; };
    services->captureAudioSamples    = [](std::chrono::milliseconds)
        -> std::optional<std::array<float, 128>> { return std::nullopt; };
    return services;
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
    if (! services->provideCefSubprocessPath) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires provideCefSubprocessPath");
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
    if (! services) {
        services = CreateDefaultWebEngineServices();
    }
    auto validation = validateWebEngineServices(services);
    if (! validation) {
        return Result<std::unique_ptr<ContentBackend>>(validation.error());
    }
    std::unique_ptr<ContentBackend> backend = std::make_unique<WebBackend>(context, std::move(services));
    return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
}
} // namespace wallpaper
