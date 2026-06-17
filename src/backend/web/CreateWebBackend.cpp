#include "backend/web/CreateWebBackend.hpp"

#include "backend/web/internal/WebBackend.hpp"

namespace wallpaper
{
Result<std::unique_ptr<ContentBackend>> CreateWebBackend(const BackendContext& context) {
    std::unique_ptr<ContentBackend> backend = std::make_unique<WebBackend>(context);
    return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
}
} // namespace wallpaper
