#include "backend/BuiltinBackendFactory.hpp"

#include "backend/scene/CreateWESceneBackend.hpp"
#include "backend/web/CreateWebBackend.hpp"

namespace wallpaper
{
namespace
{
class BuiltinBackendFactory final : public BackendFactory {
public:
    Result<std::unique_ptr<ContentBackend>> create(BackendType           type,
                                                   const BackendContext& context) override {
        switch (type) {
        case BackendType::WEScene: return CreateWESceneBackend(context);
        case BackendType::Web: return CreateWebBackend(context);
        case BackendType::Image:
        case BackendType::Video:
            return Result<std::unique_ptr<ContentBackend>>::failure(
                ResultCode::NotSupported,
                "requested backend is not registered in builtin backend factory");
        }

        return Result<std::unique_ptr<ContentBackend>>::failure(ResultCode::NotSupported,
                                                                "unknown backend type");
    }
};
} // namespace

std::shared_ptr<BackendFactory> CreateBuiltinBackendFactory() {
    return std::make_shared<BuiltinBackendFactory>();
}
} // namespace wallpaper
