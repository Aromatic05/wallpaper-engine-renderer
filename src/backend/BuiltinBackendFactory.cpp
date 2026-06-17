#include "backend/BuiltinBackendFactory.hpp"

#include "backend/scene/WESceneBackend.hpp"
#include "backend/web/WebBackend.hpp"

namespace wallpaper
{
Result<std::unique_ptr<ContentBackend>> BuiltinBackendFactory::create(BackendType           type,
                                                                      const BackendContext& context) {
    switch (type) {
    case BackendType::WEScene: {
        std::unique_ptr<ContentBackend> backend = std::make_unique<WESceneBackend>(context);
        return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
    }
    case BackendType::Web: {
        std::unique_ptr<ContentBackend> backend = std::make_unique<WebBackend>(context);
        return Result<std::unique_ptr<ContentBackend>>::success(std::move(backend));
    }
    case BackendType::Image:
    case BackendType::Video:
        return Result<std::unique_ptr<ContentBackend>>::failure(
            ResultCode::NotSupported, "requested backend is not registered in builtin backend factory");
    }

    return Result<std::unique_ptr<ContentBackend>>::failure(ResultCode::NotSupported,
                                                            "unknown backend type");
}

std::shared_ptr<BackendFactory> CreateBuiltinBackendFactory() {
    return std::make_shared<BuiltinBackendFactory>();
}
} // namespace wallpaper
