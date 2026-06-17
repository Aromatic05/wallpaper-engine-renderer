#include "backend/scene/CreateWESceneBackend.hpp"

#include "backend/scene/internal/runtime/WESceneBackend.hpp"

namespace wallpaper
{
Result<std::unique_ptr<ContentBackend>> CreateWESceneBackend(const BackendContext& context) {
    std::unique_ptr<ContentBackend> backend = std::make_unique<WESceneBackend>(context);
    return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
}
} // namespace wallpaper
