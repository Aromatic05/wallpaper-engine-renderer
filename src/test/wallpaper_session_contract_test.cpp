#include "api/WallpaperSession.hpp"
#include "common/result/Result.hpp"
#include "output/OutputTargetBinding.hpp"
#include "output/RenderPlanSource.hpp"
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
class FakeBinding final : public wallpaper::OutputTargetBinding {
public:
    wallpaper::OutputTargetBindingKind kind() const override {
        return wallpaper::OutputTargetBindingKind::Surface;
    }
};

class FakeRenderPlan final : public wallpaper::RenderPlan {
public:
    wallpaper::OutputTargetBindingKind requiredBindingKind() const override {
        return wallpaper::OutputTargetBindingKind::Surface;
    }

    std::uint64_t revision() const override { return 1; }

    wallpaper::Result<void> bindOutput(const wallpaper::OutputTarget& target) override {
        ++bindCalls;
        return target.binding && target.binding->kind() == wallpaper::OutputTargetBindingKind::Surface
                   ? wallpaper::Result<void>::success()
                   : wallpaper::Result<void>::failure(wallpaper::ResultCode::InvalidArgument,
                                                      "unexpected binding kind");
    }

    int bindCalls { 0 };
};

class FakeOutputSource final : public wallpaper::RenderPlanSource {
public:
    FakeOutputSource()
        : plan(std::make_shared<FakeRenderPlan>()) {}

    std::shared_ptr<FakeRenderPlan> renderPlanImpl() const { return plan; }

protected:
    wallpaper::Result<wallpaper::RenderPlanPtr> currentRenderPlan() const override {
        return wallpaper::Result<wallpaper::RenderPlanPtr>::success(plan);
    }

private:
    std::shared_ptr<FakeRenderPlan> plan;
};

class FakeBackend final : public wallpaper::ContentBackend {
public:
    wallpaper::BackendType type() const override { return wallpaper::BackendType::WEScene; }

    wallpaper::BackendCapabilities capabilities() const override {
        wallpaper::BackendCapabilities capabilities;
        capabilities.supportsRenderPlan = true;
        return capabilities;
    }

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

    std::shared_ptr<FakeRenderPlan> renderPlanImpl() const { return output.renderPlanImpl(); }

    void notifyOutputBound() override {
        ++notifyOutputBoundCalls;
        if (ready == wallpaper::BackendReadyState::Loaded) {
            ready = wallpaper::BackendReadyState::OutputReady;
        }
    }

    int updateCalls { 0 };
    int acquireOutputCalls { 0 };
    int tickCalls { 0 };
    int startCalls { 0 };
    int notifyOutputBoundCalls { 0 };

private:
    wallpaper::BackendReadyState ready { wallpaper::BackendReadyState::Idle };
    FakeOutputSource             output;
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

    wallpaper::OutputTarget target;
    target.type    = wallpaper::OutputTargetType::Surface;
    target.binding = std::make_shared<FakeBinding>();
    auto bindResult = session.bindOutput(target);
    assert(bindResult);
    assert(factory->lastBackend->notifyOutputBoundCalls == 1);
    assert(factory->lastBackend->renderPlanImpl()->bindCalls == 1);
    assert(session.state() == wallpaper::SessionState::OutputReady);

    auto playResult = session.play();
    assert(playResult);
    assert(factory->lastBackend->startCalls == 1);
    assert(session.state() == wallpaper::SessionState::Playing);
    return 0;
}
