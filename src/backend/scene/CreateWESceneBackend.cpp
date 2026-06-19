#include "backend/scene/CreateWESceneBackend.hpp"

#include "backend/scene/internal/resources/CreateScenePackageFs.hpp"
#include "backend/scene/internal/engine/WESceneBackend.hpp"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/PhysicalFs.h"
#include "common/fs/include/fs/VFS.h"
#include "host/HostServices.hpp"
#include "host/audio/include/audio/SoundManager.h"
#include "host/looper/include/looper/Looper.hpp"
#include "host/timer/include/timer/FrameTimer.hpp"

namespace wallpaper
{
namespace
{
std::shared_ptr<WESceneEngineServices> CreateDefaultWESceneEngineServices() {
    auto services = std::make_shared<WESceneEngineServices>();
    services->createVfs = []() {
        return std::make_unique<fs::VFS>();
    };
    services->createPhysicalFs = [](std::string_view path, bool create) {
        return std::unique_ptr<fs::Fs>(fs::CreatePhysicalFs(path, create).release());
    };
    services->createPackageFs = [](std::string_view pkgPath) {
        return CreateScenePackageFs(pkgPath);
    };
    services->createSoundManager = []() {
        return std::make_unique<audio::SoundManager>();
    };
    services->createLooper = []() {
        return std::make_shared<looper::Looper>();
    };
    services->createFrameTimer = []() {
        return std::make_unique<FrameTimer>();
    };
    return services;
}

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
    if (! target->timer.monotonicMilliseconds) {
        target->timer.monotonicMilliseconds = defaults->timer.monotonicMilliseconds;
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

void mergeMissingWESceneEngineServices(const std::shared_ptr<WESceneEngineServices>& target,
                                       const std::shared_ptr<WESceneEngineServices>& defaults) {
    if (! target || ! defaults) {
        return;
    }

    if (! target->createVfs) {
        target->createVfs = defaults->createVfs;
    }
    if (! target->createPhysicalFs) {
        target->createPhysicalFs = defaults->createPhysicalFs;
    }
    if (! target->createPackageFs) {
        target->createPackageFs = defaults->createPackageFs;
    }
    if (! target->createSoundManager) {
        target->createSoundManager = defaults->createSoundManager;
    }
    if (! target->createLooper) {
        target->createLooper = defaults->createLooper;
    }
    if (! target->createFrameTimer) {
        target->createFrameTimer = defaults->createFrameTimer;
    }
}

Result<void> validateSceneEngineServices(
    const std::shared_ptr<WESceneEngineServices>& engineServices) {
    if (! engineServices) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires engine services");
    }
    if (! engineServices->createSoundManager) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires createSoundManager");
    }
    if (! engineServices->createLooper) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires createLooper");
    }
    if (! engineServices->createVfs) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires createVfs");
    }
    if (! engineServices->createPhysicalFs) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires createPhysicalFs");
    }
    if (! engineServices->createPackageFs) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "scene backend requires createPackageFs");
    }

    return Result<void>::success();
}
} // namespace

Result<std::unique_ptr<ContentBackend>> CreateWESceneBackend(
    const BackendContext&                 context,
    std::shared_ptr<WESceneEngineServices> engineServices) {
    BackendContext effectiveContext = context;

    if (! effectiveContext.hostServices) {
        effectiveContext.hostServices = CreateDefaultHostServices();
    } else {
        mergeMissingHostServices(effectiveContext.hostServices, CreateDefaultHostServices());
    }

    if (! engineServices) {
        engineServices = CreateDefaultWESceneEngineServices();
    } else {
        mergeMissingWESceneEngineServices(engineServices, CreateDefaultWESceneEngineServices());
    }

    auto validationResult = validateSceneEngineServices(engineServices);
    if (! validationResult) {
        return Result<std::unique_ptr<ContentBackend>>(validationResult.error());
    }

    std::unique_ptr<ContentBackend> backend =
        std::make_unique<WESceneBackend>(effectiveContext, std::move(engineServices));
    return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
}
} // namespace wallpaper
