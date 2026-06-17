#include "backend/BuiltinBackendFactory.hpp"

#include "backend/scene/CreateWESceneBackend.hpp"
#include "backend/web/CreateWebBackend.hpp"

namespace wallpaper
{
Result<std::unique_ptr<ContentBackend>> BuiltinBackendFactory::create(BackendType           type,
                                                                      const BackendContext& context) {
    switch (type) {
    case BackendType::WEScene: return CreateWESceneBackend(context);
    case BackendType::Web: return CreateWebBackend(context);
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
