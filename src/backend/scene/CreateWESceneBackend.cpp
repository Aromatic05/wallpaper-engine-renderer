#include "backend/scene/CreateWESceneBackend.hpp"

#include "backend/scene/internal/CreateScenePackageFs.hpp"
#include "backend/scene/internal/engine/WESceneBackend.hpp"
#include "common/fs/include/fs/Fs.h"
#include "host/HostServices.hpp"

namespace wallpaper
{
Result<std::unique_ptr<ContentBackend>> CreateWESceneBackend(const BackendContext& context) {
    BackendContext effectiveContext = context;

    if (! effectiveContext.hostServices) {
        effectiveContext.hostServices = CreateDefaultHostServices();
    }

    if (effectiveContext.hostServices && ! effectiveContext.hostServices->fileSystem.createPackageFs) {
        effectiveContext.hostServices->fileSystem.createPackageFs = [](std::string_view pkgPath) {
            return CreateScenePackageFs(pkgPath);
        };
    }

    std::unique_ptr<ContentBackend> backend = std::make_unique<WESceneBackend>(effectiveContext);
    return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
}
} // namespace wallpaper
