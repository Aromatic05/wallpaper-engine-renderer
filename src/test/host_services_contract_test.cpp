#include "api/WallpaperSession.hpp"
#include "backend/scene/CreateWESceneBackend.hpp"
#include "host/HostServices.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/BackendFactory.hpp"
#include "runtime/backend/ContentBackend.hpp"
#include <wallpaper/scene/WESceneEngineServices.hpp>

#include <cassert>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
class MinimalOutputSource final : public wallpaper::OutputSource {
public:
    wallpaper::OutputSourceType type() const override {
        return wallpaper::OutputSourceType::Surface;
    }
};

class MinimalBackend final : public wallpaper::ContentBackend {
public:
    wallpaper::BackendType type() const override { return wallpaper::BackendType::WEScene; }
    wallpaper::BackendCapabilities capabilities() const override { return {}; }
    wallpaper::Result<void> load(const wallpaper::WallpaperSource&) override {
        return wallpaper::Result<void>::success();
    }
    wallpaper::Result<void> start() override { return wallpaper::Result<void>::success(); }
    wallpaper::Result<void> pause() override { return wallpaper::Result<void>::success(); }
    wallpaper::Result<void> resume() override { return wallpaper::Result<void>::success(); }
    wallpaper::Result<void> stop() override { return wallpaper::Result<void>::success(); }
    wallpaper::Result<void> setProperty(std::string_view, wallpaper::PropertyValue) override {
        return wallpaper::Result<void>::success();
    }
    wallpaper::Result<void> sendInput(const wallpaper::InputEvent&) override {
        return wallpaper::Result<void>::success();
    }
    wallpaper::OutputSource& outputSource() override { return output; }
    wallpaper::DiagnosticsSnapshot diagnostics() const override { return {}; }

private:
    MinimalOutputSource output;
};

class CapturingFactory final : public wallpaper::BackendFactory {
public:
    wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>> create(
        wallpaper::BackendType, const wallpaper::BackendContext& context) override {
        capturedCachePath    = context.cachePath;
        capturedHostServices = context.hostServices;
        return wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>>::success(
            std::make_unique<MinimalBackend>());
    }

    std::string                             capturedCachePath;
    std::shared_ptr<wallpaper::HostServices> capturedHostServices;
};
} // namespace

int main() {
    auto defaults = wallpaper::CreateDefaultHostServices();
    assert(defaults);
    assert(defaults->fileSystem.exists);
    assert(defaults->fileSystem.createDirectories);
    assert(defaults->timer.monotonicMilliseconds);
    assert(defaults->cache.resolveCacheRoot);
    assert(defaults->diagnostics.publish);

    auto factory      = std::make_shared<CapturingFactory>();
    auto hostServices = std::make_shared<wallpaper::HostServices>();
    hostServices->cache.resolveCacheRoot = [](std::string_view appName) {
        return std::filesystem::path("/tmp") / std::string(appName);
    };

    wallpaper::SessionConfig config;
    config.backendFactory = factory;
    config.hostServices   = hostServices;

    wallpaper::WallpaperSession session(config);
    wallpaper::WallpaperSource  source { wallpaper::BackendType::WEScene, "fake://scene", {} };
    auto                        loadResult = session.load(source);
    assert(loadResult);
    assert(factory->capturedHostServices == hostServices);
    assert(factory->capturedCachePath == "/tmp/wallpaper-engine-renderer");

    int publishedDiagnostics = 0;
    hostServices->diagnostics.publish =
        [&publishedDiagnostics](wallpaper::DiagnosticSeverity, std::string_view, std::string_view) {
            ++publishedDiagnostics;
        };

    wallpaper::SessionConfig badConfig;
    badConfig.hostServices = hostServices;
    wallpaper::WallpaperSession badSession(badConfig);
    auto badLoadResult = badSession.load(source);
    assert(! badLoadResult);
    assert(publishedDiagnostics == 1);

    wallpaper::BackendContext sceneContext;
    auto genericHostServices = std::make_shared<wallpaper::HostServices>();
    sceneContext.hostServices = genericHostServices;
    auto partialSceneServices = std::make_shared<wallpaper::WESceneEngineServices>();
    auto sceneBackendResult   = wallpaper::CreateWESceneBackend(sceneContext, partialSceneServices);
    assert(sceneBackendResult);
    assert(partialSceneServices->createSoundManager);
    assert(partialSceneServices->createLooper);
    assert(partialSceneServices->createVfs);
    assert(partialSceneServices->createPhysicalFs);
    assert(partialSceneServices->createPackageFs);
    return 0;
}
