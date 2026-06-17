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

class WrongBinding final : public wallpaper::OutputTargetBinding {
public:
    wallpaper::OutputTargetBindingKind kind() const override {
        return wallpaper::OutputTargetBindingKind::Offscreen;
    }
};

class FakeRenderPlan final : public wallpaper::RenderPlan {
public:
    explicit FakeRenderPlan(std::uint64_t revision = 1)
        : m_revision(revision) {}

    wallpaper::OutputTargetBindingKind requiredBindingKind() const override {
        return wallpaper::OutputTargetBindingKind::Surface;
    }

    std::uint64_t revision() const override { return m_revision; }

    wallpaper::Result<void> bindOutput(const wallpaper::OutputTarget& target) override {
        ++bindCalls;
        return target.binding && target.binding->kind() == wallpaper::OutputTargetBindingKind::Surface
                   ? wallpaper::Result<void>::success()
                   : wallpaper::Result<void>::failure(wallpaper::ResultCode::InvalidArgument,
                                                      "unexpected binding kind");
    }

    int bindCalls { 0 };

private:
    std::uint64_t m_revision { 1 };
};

class FakeOutputSource final : public wallpaper::RenderPlanSource {
public:
    FakeOutputSource()
        : plan(std::make_shared<FakeRenderPlan>()) {}

    std::shared_ptr<FakeRenderPlan> renderPlanImpl() const { return plan; }
    void setRenderPlan(std::shared_ptr<FakeRenderPlan> nextPlan) { plan = std::move(nextPlan); }

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
        if (nextPlan) {
            output.setRenderPlan(nextPlan);
            nextPlan.reset();
        }

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
    void setNextRenderPlan(std::shared_ptr<FakeRenderPlan> plan) { nextPlan = std::move(plan); }

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
    std::shared_ptr<FakeRenderPlan> nextPlan;
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

    wallpaper::OutputTarget invalidTarget;
    auto invalidBindResult = session.bindOutput(invalidTarget);
    assert(! invalidBindResult);
    assert(invalidBindResult.error().code == wallpaper::ResultCode::InvalidArgument);

    wallpaper::WallpaperSource source { wallpaper::BackendType::WEScene, "fake://scene", {} };
    auto loadResult = session.load(source);
    assert(loadResult);
    assert(session.state() == wallpaper::SessionState::Loading);
    assert(session.readyState() == wallpaper::BackendReadyState::Loading);
    assert(session.playbackState() == wallpaper::PlaybackState::Idle);
    assert(factory->lastBackend != nullptr);

    auto playWhileLoadingResult = session.play();
    assert(playWhileLoadingResult);
    assert(factory->lastBackend->startCalls == 1);
    assert(session.state() == wallpaper::SessionState::Playing);
    assert(session.readyState() == wallpaper::BackendReadyState::Loading);
    assert(session.playbackState() == wallpaper::PlaybackState::Playing);

    auto lifecycleResult = session.tick();
    assert(lifecycleResult);
    assert(lifecycleResult.value().contentStateChanged);
    assert(lifecycleResult.value().frameRequested);
    assert(factory->lastBackend->updateCalls == 1);
    assert(factory->lastBackend->acquireOutputCalls == 1);
    assert(factory->lastBackend->tickCalls == 1);
    assert(session.state() == wallpaper::SessionState::Playing);
    assert(session.readyState() == wallpaper::BackendReadyState::Loaded);
    assert(session.playbackState() == wallpaper::PlaybackState::Playing);

    wallpaper::OutputTarget target;
    target.type    = wallpaper::OutputTargetType::Surface;
    target.binding = std::make_shared<FakeBinding>();
    auto bindResult = session.bindOutput(target);
    assert(bindResult);
    assert(factory->lastBackend->notifyOutputBoundCalls == 1);
    assert(factory->lastBackend->renderPlanImpl()->bindCalls == 1);
    assert(session.state() == wallpaper::SessionState::Playing);
    assert(session.readyState() == wallpaper::BackendReadyState::OutputReady);

    wallpaper::OutputTarget wrongTarget;
    wrongTarget.type    = wallpaper::OutputTargetType::Surface;
    wrongTarget.binding = std::make_shared<WrongBinding>();
    auto failedRebindResult = session.bindOutput(wrongTarget);
    assert(! failedRebindResult);
    assert(failedRebindResult.error().code == wallpaper::ResultCode::InvalidArgument);
    assert(factory->lastBackend->notifyOutputBoundCalls == 1);
    assert(factory->lastBackend->renderPlanImpl()->bindCalls == 1);

    auto replacementPlan = std::make_shared<FakeRenderPlan>(2);
    factory->lastBackend->setNextRenderPlan(replacementPlan);
    auto rebindLifecycleResult = session.tick();
    assert(rebindLifecycleResult);
    assert(factory->lastBackend->acquireOutputCalls == 2);
    assert(factory->lastBackend->notifyOutputBoundCalls == 2);
    assert(replacementPlan->bindCalls == 1);
    assert(session.state() == wallpaper::SessionState::Playing);
    assert(session.readyState() == wallpaper::BackendReadyState::OutputReady);
    return 0;
}
