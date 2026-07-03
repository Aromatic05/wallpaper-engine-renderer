#include "backend/web/internal/BrowserHostLoader.hpp"

#include "utils/DynamicLibrary.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>

namespace wallpaper
{
namespace
{
using BrowserHostFactoryFn = std::shared_ptr<WebBrowserHost> (*)();

struct BrowserHostRuntimeModule {
    utils::DynamicLibrary cefLibrary;
    utils::DynamicLibrary browserHostLibrary;
    BrowserHostFactoryFn  create { nullptr };
    std::string           error;
};

std::filesystem::path currentExecutableDir() {
    std::array<char, PATH_MAX> buf {};
    const auto length = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (length <= 0) return {};
    buf[static_cast<std::size_t>(length)] = '\0';
    return std::filesystem::path(buf.data()).parent_path();
}

std::filesystem::path currentLibraryDir() {
    Dl_info info {};
    if (::dladdr(reinterpret_cast<void*>(&CreateWebBrowserHostRuntime), &info) == 0 || ! info.dli_fname) {
        return {};
    }
    return std::filesystem::path(info.dli_fname).parent_path();
}

std::vector<std::filesystem::path> candidateCefLibraries() {
    std::vector<std::filesystem::path> candidates;
    if (const char* value = std::getenv("WE_CEF_LIBRARY"); value && *value) {
        candidates.emplace_back(value);
    }
    if (const char* value = std::getenv("CEF_ROOT"); value && *value) {
        const std::filesystem::path root { value };
        candidates.push_back(root / "Release" / "libcef.so");
        candidates.push_back(root / "libcef.so");
    }
    candidates.emplace_back("/usr/lib/cef/libcef.so");
    candidates.emplace_back("/usr/local/lib/cef/libcef.so");
    return candidates;
}

std::vector<std::filesystem::path> candidateBrowserHostLibraries() {
    std::vector<std::filesystem::path> candidates;
    const auto exeDir = currentExecutableDir();
    const auto libDir = currentLibraryDir();

    if (! libDir.empty()) {
        candidates.push_back(libDir / "backend" / "web" / "libwpWebBrowserHost.so");
        candidates.push_back(libDir / "wallpaper-engine-renderer" / "libwpWebBrowserHost.so");
        candidates.push_back(libDir / "libwpWebBrowserHost.so");
    }
    if (! exeDir.empty()) {
        candidates.push_back(exeDir / ".." / "src" / "backend" / "web" / "libwpWebBrowserHost.so");
        candidates.push_back(exeDir / ".." / "backend" / "web" / "libwpWebBrowserHost.so");
        candidates.push_back(exeDir / "libwpWebBrowserHost.so");
    }
    return candidates;
}

bool loadSharedLibrary(utils::DynamicLibrary&                   library,
                       const std::vector<std::filesystem::path>& candidates,
                       int                                       flags,
                       std::string&                              error,
                       const char*                               label) {
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (! std::filesystem::is_regular_file(candidate, ec) || ec) continue;
        if (library.Open(candidate.c_str(), flags)) return true;
        if (const char* dlerror_message = ::dlerror()) {
            error = std::string(label) + " load failed for " + candidate.string() + ": "
                    + dlerror_message;
        }
    }
    if (error.empty()) {
        error = std::string("could not locate ") + label;
    }
    return false;
}

Result<BrowserHostFactoryFn> loadBrowserHostFactory() {
    static BrowserHostRuntimeModule module = [] {
        BrowserHostRuntimeModule runtime;
        if (! loadSharedLibrary(runtime.cefLibrary,
                                candidateCefLibraries(),
                                RTLD_NOW | RTLD_GLOBAL,
                                runtime.error,
                                "libcef.so")) {
            return runtime;
        }
        if (! loadSharedLibrary(runtime.browserHostLibrary,
                                candidateBrowserHostLibraries(),
                                RTLD_NOW | RTLD_LOCAL,
                                runtime.error,
                                "libwpWebBrowserHost.so")) {
            return runtime;
        }
        if (! runtime.browserHostLibrary.GetSymbol("wallpaper_create_web_browser_host",
                                                   runtime.create)) {
            runtime.error =
                "libwpWebBrowserHost.so is missing wallpaper_create_web_browser_host";
        }
        return runtime;
    }();

    if (! module.create) {
        return Result<BrowserHostFactoryFn>::failure(ResultCode::NotFound, module.error);
    }
    return Result<BrowserHostFactoryFn>::success(module.create);
}
} // namespace

Result<std::shared_ptr<WebBrowserHost>> CreateWebBrowserHostRuntime() {
    auto factoryResult = loadBrowserHostFactory();
    if (! factoryResult) {
        return Result<std::shared_ptr<WebBrowserHost>>(factoryResult.error());
    }
    auto host = factoryResult.value()();
    if (! host) {
        return Result<std::shared_ptr<WebBrowserHost>>::failure(
            ResultCode::InternalError, "libwpWebBrowserHost.so returned a null WebBrowserHost");
    }
    return Result<std::shared_ptr<WebBrowserHost>>::success(std::move(host));
}
} // namespace wallpaper
