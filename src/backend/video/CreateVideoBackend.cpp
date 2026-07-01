#include "backend/video/CreateVideoBackend.hpp"

#include "backend/video/internal/VideoBackend.hpp"

namespace wallpaper
{
Result<std::unique_ptr<ContentBackend>> CreateVideoBackend(const BackendContext& context) {
    return Result<std::unique_ptr<ContentBackend>>::success(
        std::make_unique<VideoBackend>(context));
}
} // namespace wallpaper
