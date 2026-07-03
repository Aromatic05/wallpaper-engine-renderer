#include "backend/web/CreateWebBackend.hpp"

#include "backend/web/internal/WebBackend.hpp"
#include "wallpaper/web/WebEngineServices.hpp"

#include "host/HostServices.hpp"

#include <cstdlib>
#include <limits.h>
#include <unistd.h>
#include <vector>

#include <utility>

namespace wallpaper
{
namespace
{
WebCefRuntimeProfile runtimeProfileFromEnv() {
    if (const char* value = std::getenv("WE_CEF_PROFILE")) {
        const std::string profile { value };
        if (profile == "compat" || profile == "compatibility") {
            return WebCefRuntimeProfile::Compatibility;
        }
        if (profile == "debug") {
            return WebCefRuntimeProfile::Debug;
        }
    }
    return WebCefRuntimeProfile::Default;
}

WebCefWindowSystem preferredWindowSystemFromEnv() {
    if (const char* value = std::getenv("WE_CEF_WINDOW_SYSTEM")) {
        const std::string system { value };
        if (system == "x11") {
            return WebCefWindowSystem::X11;
        }
        if (system == "wayland") {
            return WebCefWindowSystem::Wayland;
        }
    }
    return WebCefWindowSystem::Auto;
}

std::vector<std::string> extraSwitchesFromEnv() {
    std::vector<std::string> switches;
    if (const char* value = std::getenv("WE_CEF_EXTRA_SWITCHES")) {
        std::string current;
        for (const char ch : std::string(value)) {
            if (ch == ',' || ch == ';' || ch == '\n') {
                if (! current.empty()) {
                    switches.push_back(current);
                    current.clear();
                }
                continue;
            }
            current.push_back(ch);
        }
        if (! current.empty()) {
            switches.push_back(current);
        }
    }
    return switches;
}

std::filesystem::path helperPathFromEnv() {
    if (const char* value = std::getenv("WE_CEF_HELPER_PATH")) {
        if (*value) return value;
    }
    return {};
}

std::filesystem::path userLocalLibDir() {
    if (const char* home = std::getenv("HOME")) {
        if (*home) return std::filesystem::path(home) / ".local" / "lib";
    }
    return {};
}

std::filesystem::path resolveDefaultCefHelperPath();

std::filesystem::path currentExecutableDir() {
    std::array<char, PATH_MAX> buf {};
    const auto length = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (length <= 0) return {};
    buf[static_cast<std::size_t>(length)] = '\0';
    return std::filesystem::path(buf.data()).parent_path();
}

std::filesystem::path firstExistingDirectory(const std::vector<std::filesystem::path>& candidates) {
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        ec.clear();
        if (std::filesystem::exists(candidate, ec) && ! ec &&
            std::filesystem::is_directory(candidate, ec) && ! ec) {
            return candidate;
        }
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

std::filesystem::path resolveBundledCefResourcesDir() {
    const auto helperPath = resolveDefaultCefHelperPath();
    if (helperPath.empty()) return {};
    const auto helperDir = helperPath.parent_path();
    return firstExistingDirectory({ helperDir / "cef" });
}

std::filesystem::path resolveBundledCefLocalesDir() {
    const auto helperPath = resolveDefaultCefHelperPath();
    if (helperPath.empty()) return {};
    const auto helperDir = helperPath.parent_path();
    return firstExistingDirectory({ helperDir / "cef" / "locales" });
}

std::filesystem::path resolveDefaultCefResourcesDir() {
    if (auto bundled = resolveBundledCefResourcesDir(); ! bundled.empty()) {
        return bundled;
    }

    if (const char* cefRoot = std::getenv("CEF_ROOT")) {
        if (*cefRoot) {
            const std::filesystem::path root { cefRoot };
            auto path = firstExistingDirectory({ root / "Resources", root });
            if (! path.empty()) return path;
        }
    }

    return firstExistingDirectory({
        userLocalLibDir() / "cef" / "Resources",
        userLocalLibDir() / "cef",
        "/usr/lib/cef/Resources",
        "/usr/lib/cef",
    });
}

std::filesystem::path resolveDefaultCefLocalesDir() {
    if (auto bundled = resolveBundledCefLocalesDir(); ! bundled.empty()) {
        return bundled;
    }

    if (const char* cefRoot = std::getenv("CEF_ROOT")) {
        if (*cefRoot) {
            const std::filesystem::path root { cefRoot };
            auto path = firstExistingDirectory({ root / "Resources" / "locales", root / "locales" });
            if (! path.empty()) return path;
        }
    }

    return firstExistingDirectory({
        userLocalLibDir() / "cef" / "Resources" / "locales",
        userLocalLibDir() / "cef" / "locales",
        "/usr/lib/cef/Resources/locales",
        "/usr/lib/cef/locales",
    });
}

std::filesystem::path resolveDefaultCefHelperPath() {
    if (auto fromEnv = helperPathFromEnv(); ! fromEnv.empty()) {
        return fromEnv;
    }

    std::vector<std::filesystem::path> candidates;
    const auto executableDir = currentExecutableDir();
    if (const char* cefRoot = std::getenv("CEF_ROOT")) {
        if (*cefRoot) {
            const std::filesystem::path root { cefRoot };
            candidates.push_back(root / "Release" / "we-cef-helper");
            candidates.push_back(root / "we-cef-helper");
        }
    }

    candidates.push_back(executableDir / ".." / "lib" / "we-cef-helper");
    if (const auto localLibDir = userLocalLibDir(); ! localLibDir.empty()) {
        candidates.push_back(localLibDir / "we-cef-helper");
    }
    candidates.push_back(std::filesystem::path("/usr/lib/we-cef-helper"));
    candidates.push_back(executableDir / "we-cef-helper");
    candidates.push_back(executableDir / ".." / "backend" / "web" / "we-cef-helper");
    candidates.push_back(executableDir / ".." / ".." / "backend" / "web" / "we-cef-helper");
    candidates.push_back(std::filesystem::current_path() / "we-cef-helper");
    candidates.push_back(std::filesystem::current_path() / "build" / "src" / "backend" / "web" / "we-cef-helper");
    candidates.push_back(std::filesystem::current_path() / "build-check" / "src" / "backend" / "web" / "we-cef-helper");
    candidates.push_back(std::filesystem::current_path() / "build-web-check" / "src" / "backend" / "web" / "we-cef-helper");
    return firstExistingRegularFile(candidates);
}

std::shared_ptr<WebEngineServices> CreateDefaultWebEngineServicesImpl(const BackendContext& context) {
    auto services = std::make_shared<WebEngineServices>();
    services->provideCefResourcesDir = []() -> std::filesystem::path {
        return resolveDefaultCefResourcesDir();
    };
    services->provideCefLocalesDir   = []() -> std::filesystem::path {
        return resolveDefaultCefLocalesDir();
    };
    services->provideCefCacheDir     = [cache_path = context.cachePath]() -> std::filesystem::path {
        if (! cache_path.empty()) {
            return std::filesystem::path(cache_path) / "web-cef";
        }
        return std::filesystem::temp_directory_path() / "wallpaper-engine-renderer" / "cef-cache";
    };
    services->provideCefSubprocessPath = []() -> std::filesystem::path {
        return resolveDefaultCefHelperPath();
    };
    services->runtimeProfile = []() {
        return runtimeProfileFromEnv();
    };
    services->preferredWindowSystem = []() {
        return preferredWindowSystemFromEnv();
    };
    services->extraCommandLineSwitches = []() {
        return extraSwitchesFromEnv();
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
    if (! services->runtimeProfile) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires runtimeProfile");
    }
    if (! services->preferredWindowSystem) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires preferredWindowSystem");
    }
    if (! services->extraCommandLineSwitches) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires extraCommandLineSwitches");
    }
    if (! services->captureAudioSamples) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "web backend requires captureAudioSamples");
    }
    return Result<void>::success();
}
} // namespace

std::shared_ptr<WebEngineServices> CreateDefaultWebEngineServices() {
    return CreateDefaultWebEngineServicesImpl(BackendContext {});
}

Result<std::unique_ptr<ContentBackend>> CreateWebBackend(const BackendContext&              context,
                                                          std::shared_ptr<WebEngineServices> services) {
    if (! services) {
        services = CreateDefaultWebEngineServicesImpl(context);
    }
    auto validation = validateWebEngineServices(services);
    if (! validation) {
        return Result<std::unique_ptr<ContentBackend>>(validation.error());
    }
    std::unique_ptr<ContentBackend> backend = std::make_unique<WebBackend>(context, std::move(services));
    return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
}
} // namespace wallpaper
