#include "backend/BuiltinBackendFactory.hpp"

#include "backend/scene/CreateWESceneBackend.hpp"

#if BUILD_WEWEB
#include "backend/web/CreateWebBackend.hpp"
#endif

namespace wallpaper
{
namespace
{
class BuiltinBackendFactory final : public BackendFactory {
public:
    Result<std::unique_ptr<ContentBackend>> create(BackendType           type,
                                                   const BackendContext& context) override {
        switch (type) {
        case BackendType::WEScene:
            return CreateWESceneBackend(context);
#if BUILD_WEWEB
        case BackendType::Web:
            return CreateWebBackend(context);
#endif
        case BackendType::Image:
        case BackendType::Video:
            return Result<std::unique_ptr<ContentBackend>>::failure(
                ResultCode::NotSupported,
                "requested backend is not registered in builtin backend factory");
        }

        // Web backend is the only case that needs a non-fallthrough
        // default when BUILD_WEWEB=OFF.
#if !BUILD_WEWEB
        if (type == BackendType::Web) {
            return Result<std::unique_ptr<ContentBackend>>::failure(
                ResultCode::NotSupported,
                "web backend is not built in this configuration (rebuild with -DBUILD_WEWEB=ON)");
        }
#endif

        return Result<std::unique_ptr<ContentBackend>>::failure(ResultCode::NotSupported,
                                                                "unknown backend type");
    }
};
} // namespace

std::shared_ptr<BackendFactory> CreateBuiltinBackendFactory() {
    return std::make_shared<BuiltinBackendFactory>();
}
} // namespace wallpaper
