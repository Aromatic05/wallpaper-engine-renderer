#include "backend/scene/CreateWESceneBackend.hpp"

#include "backend/scene/internal/CreateScenePackageFs.hpp"
#include "backend/scene/internal/engine/WESceneBackend.hpp"
#include "common/fs/include/fs/Fs.h"
#include "host/HostServices.hpp"

namespace wallpaper
{
namespace
{
void mergeMissingHostServices(const std::shared_ptr<HostServices>& target,
                              const std::shared_ptr<HostServices>& defaults) {
    if (! target || ! defaults) {
        return;
    }

    if (! target->fileSystem.exists) {
        target->fileSystem.exists = defaults->fileSystem.exists;
    }
    if (! target->fileSystem.createDirectories) {
        target->fileSystem.createDirectories = defaults->fileSystem.createDirectories;
    }
    if (! target->fileSystem.createVfs) {
        target->fileSystem.createVfs = defaults->fileSystem.createVfs;
    }
    if (! target->fileSystem.createPhysicalFs) {
        target->fileSystem.createPhysicalFs = defaults->fileSystem.createPhysicalFs;
    }
    if (! target->fileSystem.createPackageFs) {
        target->fileSystem.createPackageFs = defaults->fileSystem.createPackageFs;
    }
    if (! target->audio.createSoundManager) {
        target->audio.createSoundManager = defaults->audio.createSoundManager;
    }
    if (! target->timer.monotonicMilliseconds) {
        target->timer.monotonicMilliseconds = defaults->timer.monotonicMilliseconds;
    }
    if (! target->timer.createLooper) {
        target->timer.createLooper = defaults->timer.createLooper;
    }
    if (! target->timer.createFrameTimer) {
        target->timer.createFrameTimer = defaults->timer.createFrameTimer;
    }
    if (! target->platform.cachePathForApp) {
        target->platform.cachePathForApp = defaults->platform.cachePathForApp;
    }
    if (! target->cache.resolveCacheRoot) {
        target->cache.resolveCacheRoot = defaults->cache.resolveCacheRoot;
    }
    if (! target->diagnostics.publish) {
        target->diagnostics.publish = defaults->diagnostics.publish;
    }
}

Result<void> validateSceneHostServices(const std::shared_ptr<HostServices>& hostServices) {
    if (! hostServices) {
        return Result<void>::failure(ResultCode::InvalidArgument, "scene backend requires host services");
    }
    if (! hostServices->audio.createSoundManager) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires audio.createSoundManager");
    }
    if (! hostServices->timer.createLooper) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires timer.createLooper");
    }
    if (! hostServices->fileSystem.createVfs) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires fileSystem.createVfs");
    }
    if (! hostServices->fileSystem.createPhysicalFs) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires fileSystem.createPhysicalFs");
    }
    if (! hostServices->fileSystem.createPackageFs) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires fileSystem.createPackageFs");
    }

    return Result<void>::success();
}
} // namespace

Result<std::unique_ptr<ContentBackend>> CreateWESceneBackend(const BackendContext& context) {
    BackendContext effectiveContext = context;

    if (! effectiveContext.hostServices) {
        effectiveContext.hostServices = CreateDefaultHostServices();
    } else {
        mergeMissingHostServices(effectiveContext.hostServices, CreateDefaultHostServices());
    }

    if (effectiveContext.hostServices && ! effectiveContext.hostServices->fileSystem.createPackageFs) {
        effectiveContext.hostServices->fileSystem.createPackageFs = [](std::string_view pkgPath) {
            return CreateScenePackageFs(pkgPath);
        };
    }

    auto validationResult = validateSceneHostServices(effectiveContext.hostServices);
    if (! validationResult) {
        return Result<std::unique_ptr<ContentBackend>>(validationResult.error());
    }

    std::unique_ptr<ContentBackend> backend = std::make_unique<WESceneBackend>(effectiveContext);
    return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
}
} // namespace wallpaper
