#include "api/WallpaperSession.hpp"
#include "common/result/Result.hpp"
#include "output/OutputSource.hpp"
#include "runtime/backend/BackendCapabilities.hpp"
#include "runtime/backend/BackendContext.hpp"
#include "runtime/backend/BackendFactory.hpp"
#include "runtime/backend/BackendReadyState.hpp"
#include "runtime/backend/ContentBackend.hpp"

#include <cassert>
#include <memory>
#include <string_view>

namespace
{
class NullOutputSource final : public wallpaper::OutputSource {
public:
    wallpaper::OutputSourceType type() const override {
        return wallpaper::OutputSourceType::Surface;
    }
};

class FakeBackend final : public wallpaper::ContentBackend {
public:
    wallpaper::BackendType type() const override { return wallpaper::BackendType::WEScene; }

    wallpaper::BackendCapabilities capabilities() const override { return {}; }

    wallpaper::Result<void> load(const wallpaper::WallpaperSource&) override {
        ready = wallpaper::BackendReadyState::Loading;
        return wallpaper::Result<void>::success();
    }

    wallpaper::Result<void> start() override {
        ++startCalls;
        return wallpaper::Result<void>::success();
    }

    wallpaper::Result<void> pause() override { return wallpaper::Result<void>::success(); }

    wallpaper::Result<void> resume() override { return wallpaper::Result<void>::success(); }

    wallpaper::Result<void> stop() override { return wallpaper::Result<void>::success(); }

    wallpaper::Result<void> setProperty(std::string_view, wallpaper::PropertyValue) override {
        return wallpaper::Result<void>::success();
    }

    wallpaper::Result<void> sendInput(const wallpaper::InputEvent&) override {
        return wallpaper::Result<void>::success();
    }

    wallpaper::Result<void> update() override {
        ++updateCalls;
        return wallpaper::Result<void>::success();
    }

    wallpaper::Result<wallpaper::OutputSource*> acquireOutput() override {
        ++acquireOutputCalls;
        return wallpaper::Result<wallpaper::OutputSource*>::success(&output);
    }

    wallpaper::Result<wallpaper::FrameLifecycle> tick() override {
        ++tickCalls;
        ready = wallpaper::BackendReadyState::Loaded;

        wallpaper::FrameLifecycle lifecycle;
        lifecycle.contentStateChanged = true;
        lifecycle.frameRequested      = true;
        return wallpaper::Result<wallpaper::FrameLifecycle>::success(lifecycle);
    }

    bool loadsAsynchronously() const override { return true; }

    wallpaper::BackendReadyState readyState() const override { return ready; }

    wallpaper::OutputSource& outputSource() override { return output; }

    wallpaper::DiagnosticsSnapshot diagnostics() const override { return {}; }

    int updateCalls { 0 };
    int acquireOutputCalls { 0 };
    int tickCalls { 0 };
    int startCalls { 0 };

private:
    wallpaper::BackendReadyState ready { wallpaper::BackendReadyState::Idle };
    NullOutputSource             output;
};

class FakeFactory final : public wallpaper::BackendFactory {
public:
    wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>> create(
        wallpaper::BackendType, const wallpaper::BackendContext&) override {
        auto backend = std::make_unique<FakeBackend>();
        lastBackend  = backend.get();
        return wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>>::success(
            std::move(backend));
    }

    FakeBackend* lastBackend { nullptr };
};
} // namespace

int main() {
    auto factory = std::make_shared<FakeFactory>();

    wallpaper::SessionConfig config;
    config.backendFactory = factory;

    wallpaper::WallpaperSession session(config);

    wallpaper::WallpaperSource source { wallpaper::BackendType::WEScene, "fake://scene", {} };
    auto loadResult = session.load(source);
    assert(loadResult);
    assert(session.state() == wallpaper::SessionState::Loading);
    assert(factory->lastBackend != nullptr);

    auto lifecycleResult = session.tick();
    assert(lifecycleResult);
    assert(lifecycleResult.value().contentStateChanged);
    assert(lifecycleResult.value().frameRequested);
    assert(factory->lastBackend->updateCalls == 1);
    assert(factory->lastBackend->acquireOutputCalls == 1);
    assert(factory->lastBackend->tickCalls == 1);
    assert(session.state() == wallpaper::SessionState::Loaded);

    auto playResult = session.play();
    assert(playResult);
    assert(factory->lastBackend->startCalls == 1);
    assert(session.state() == wallpaper::SessionState::Playing);
    return 0;
}
