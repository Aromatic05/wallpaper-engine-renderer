#include <wallpaper/BackendContext.hpp>
#include <wallpaper/BackendFactory.hpp>
#include <wallpaper/ContentBackend.hpp>
#include <wallpaper/HostServices.hpp>
#include <wallpaper/OutputSource.hpp>
#include <wallpaper/WallpaperRuntime.hpp>
#include <wallpaper/WallpaperSession.hpp>
#include <wallpaper/WallpaperTypes.hpp>
#include <wallpaper/scene/WEScene.hpp>
#include <wallpaper/scene/WESceneOutput.hpp>
#include <wallpaper/scene/WESceneSource.hpp>
#include <wallpaper/web/Web.hpp>

#include <memory>

namespace
{
class PublicOutputSource final : public wallpaper::OutputSource {
public:
    wallpaper::Result<wallpaper::RenderPlanPtr> renderPlan() const override {
        return wallpaper::Result<wallpaper::RenderPlanPtr>::failure(
            wallpaper::ResultCode::InvalidState,
            "public API test backend has no loaded render plan");
    }
};

class PublicBackend final : public wallpaper::ContentBackend {
public:
    wallpaper::BackendType type() const override { return wallpaper::BackendType::WEScene; }
    wallpaper::BackendCapabilities capabilities() const override { return {}; }
    wallpaper::Result<void> load(const wallpaper::WallpaperSource&) override {
        return wallpaper::Result<void>::success();
    }
    wallpaper::Result<void> start() override { return wallpaper::Result<void>::success(); }
    wallpaper::Result<void> pause() override { return wallpaper::Result<void>::success(); }
    wallpaper::Result<void> resume() override { return wallpaper::Result<void>::success(); }
    wallpaper::Result<void> stop() override { return wallpaper::Result<void>::success(); }
    wallpaper::Result<void> setProperty(std::string_view, wallpaper::PropertyValue) override {
        return wallpaper::Result<void>::success();
    }
    wallpaper::Result<void> sendInput(const wallpaper::InputEvent&) override {
        return wallpaper::Result<void>::success();
    }
    wallpaper::OutputSource& outputSource() override { return output; }
    wallpaper::DiagnosticsSnapshot diagnostics() const override { return {}; }

private:
    PublicOutputSource output;
};

class PublicFactory final : public wallpaper::BackendFactory {
public:
    wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>> create(
        wallpaper::BackendType, const wallpaper::BackendContext&) override {
        return wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>>::success(
            std::make_unique<PublicBackend>());
    }
};
} // namespace

int main() {
    wallpaper::SessionConfig   config;
    wallpaper::WallpaperSource source { wallpaper::BackendType::WEScene, "demo://scene", {} };
    config.backendFactory = std::make_shared<PublicFactory>();
    config.hostServices   = std::make_shared<wallpaper::HostServices>();

    auto runtime = std::make_unique<wallpaper::WallpaperRuntime>();
    auto session = runtime->createSession(config);
    static_assert(wallpaper::WE_SCENE_PROPERTY_LOAD_USER_PROPERTIES == "load_user_properties");
    static_assert(wallpaper::WE_SCENE_PROPERTY_USER_PROPERTIES == "user_properties");
    static_assert(wallpaper::WE_SCENE_PROPERTY_AUDIO_SAMPLES == "audio_samples");
    static_assert(wallpaper::WE_SCENE_PROPERTY_MEDIA_STATE == "media_state");
    static_assert(wallpaper::WE_SCENE_PROPERTY_CAPTURE_FRAME == "capture_frame");
    static_assert(wallpaper::WE_SCENE_PROPERTY_CAPTURE_FRAME_NUMBER == "capture_frame_number");
    (void)source;
    (void)session;
    return 0;
}
