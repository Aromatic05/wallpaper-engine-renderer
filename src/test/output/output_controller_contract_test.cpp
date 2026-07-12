#include "api/scene/WESceneOutput.hpp"
#include "output/OutputController.hpp"
#include "output/RenderPlanSource.hpp"

#include <cassert>
#include <cstddef>
#include <memory>

namespace
{
class FakeSceneRenderPlan final : public wallpaper::RenderPlan {
public:
    explicit FakeSceneRenderPlan(std::uint64_t revision, bool shouldFail = false)
        : m_revision(revision)
        , m_shouldFail(shouldFail) {}

    wallpaper::OutputTargetBindingKind requiredBindingKind() const override {
        return wallpaper::OutputTargetBindingKind::VulkanRenderTarget;
    }

    std::uint64_t revision() const override { return m_revision; }

    wallpaper::Result<void> bindOutput(const wallpaper::OutputTarget& target) override {
        auto binding = std::dynamic_pointer_cast<wallpaper::WESceneOutputBinding>(target.binding);
        assert(binding);
        ++bindCalls;
        prepared = true;
        if (m_shouldFail) {
            return wallpaper::Result<void>::failure(wallpaper::ResultCode::InternalError,
                                                    "bind failed");
        }
        return wallpaper::Result<void>::success();
    }

    int  bindCalls { 0 };
    bool prepared { false };

private:
    std::uint64_t m_revision { 0 };
    bool          m_shouldFail { false };
};

class FakeRenderPlanSource final : public wallpaper::RenderPlanSource {
public:
    explicit FakeRenderPlanSource(std::shared_ptr<FakeSceneRenderPlan> plan)
        : plan(std::move(plan)) {}

protected:
    wallpaper::Result<wallpaper::RenderPlanPtr> currentRenderPlan() const override {
        return wallpaper::Result<wallpaper::RenderPlanPtr>::success(plan);
    }

private:
    std::shared_ptr<FakeSceneRenderPlan> plan;
};

class SequentialRenderPlanSource final : public wallpaper::RenderPlanSource {
public:
    SequentialRenderPlanSource(std::shared_ptr<FakeSceneRenderPlan> firstPlan,
                               std::shared_ptr<FakeSceneRenderPlan> secondPlan)
        : m_firstPlan(std::move(firstPlan))
        , m_secondPlan(std::move(secondPlan)) {}

protected:
    wallpaper::Result<wallpaper::RenderPlanPtr> currentRenderPlan() const override {
        ++calls;
        if (calls == 1) {
            return wallpaper::Result<wallpaper::RenderPlanPtr>::success(m_firstPlan);
        }
        return wallpaper::Result<wallpaper::RenderPlanPtr>::success(m_secondPlan);
    }

private:
    mutable int                            calls { 0 };
    std::shared_ptr<FakeSceneRenderPlan> m_firstPlan;
    std::shared_ptr<FakeSceneRenderPlan> m_secondPlan;
};

class LifetimeTrackedRenderPlan final : public wallpaper::RenderPlan {
public:
    explicit LifetimeTrackedRenderPlan(std::size_t& destructionCount)
        : m_destructionCount(destructionCount) {}

    ~LifetimeTrackedRenderPlan() override { ++m_destructionCount; }

    wallpaper::OutputTargetBindingKind requiredBindingKind() const override {
        return wallpaper::OutputTargetBindingKind::VulkanRenderTarget;
    }

    std::uint64_t revision() const override { return 23; }

    wallpaper::Result<void> bindOutput(const wallpaper::OutputTarget&) override {
        ++bindCalls;
        return wallpaper::Result<void>::success();
    }

    int bindCalls { 0 };

private:
    std::size_t& m_destructionCount;
};

class EphemeralRenderPlanSource final : public wallpaper::RenderPlanSource {
public:
    explicit EphemeralRenderPlanSource(std::size_t& destructionCount)
        : m_destructionCount(destructionCount) {}

    std::weak_ptr<LifetimeTrackedRenderPlan> lastPlan() const { return m_lastPlan; }

protected:
    wallpaper::Result<wallpaper::RenderPlanPtr> currentRenderPlan() const override {
        auto plan = std::make_shared<LifetimeTrackedRenderPlan>(m_destructionCount);
        m_lastPlan = plan;
        return wallpaper::Result<wallpaper::RenderPlanPtr>::success(plan);
    }

private:
    std::size_t&                                   m_destructionCount;
    mutable std::weak_ptr<LifetimeTrackedRenderPlan> m_lastPlan;
};

class WrongBinding final : public wallpaper::OutputTargetBinding {
public:
    wallpaper::OutputTargetBindingKind kind() const override {
        return wallpaper::OutputTargetBindingKind::Surface;
    }
};

class SurfaceRenderPlan final : public wallpaper::RenderPlan {
public:
    wallpaper::OutputTargetBindingKind requiredBindingKind() const override {
        return wallpaper::OutputTargetBindingKind::Surface;
    }

    std::uint64_t revision() const override { return 3; }

    wallpaper::Result<void> bindOutput(const wallpaper::OutputTarget& target) override {
        assert(target.binding->kind() == wallpaper::OutputTargetBindingKind::Surface);
        prepared = true;
        return wallpaper::Result<void>::success();
    }

    bool prepared { false };
};

class SurfaceRenderPlanSource final : public wallpaper::RenderPlanSource {
public:
    explicit SurfaceRenderPlanSource(std::shared_ptr<SurfaceRenderPlan> plan)
        : plan(std::move(plan)) {}

protected:
    wallpaper::Result<wallpaper::RenderPlanPtr> currentRenderPlan() const override {
        return wallpaper::Result<wallpaper::RenderPlanPtr>::success(plan);
    }

private:
    std::shared_ptr<SurfaceRenderPlan> plan;
};
} // namespace

int main() {
    auto plan   = std::make_shared<FakeSceneRenderPlan>(7);
    auto source = FakeRenderPlanSource(plan);

    wallpaper::OutputController controller;

    wallpaper::OutputTarget wrongTarget;
    wrongTarget.type    = wallpaper::OutputTargetType::Surface;
    wrongTarget.binding = std::make_shared<WrongBinding>();
    auto wrongResult    = controller.bind(wrongTarget, source);
    assert(! wrongResult);
    assert(! plan->prepared);

    wallpaper::RenderInitInfo initInfo;
    auto                      sceneBinding = wallpaper::MakeWESceneOutputBinding(initInfo);
    auto                      goodTarget   = wallpaper::MakeWESceneOutputTarget(sceneBinding);
    auto                      goodResult   = controller.bind(goodTarget, source);
    assert(goodResult);
    assert(plan->prepared);
    assert(controller.boundRenderPlanRevision().has_value());
    assert(controller.boundRenderPlanRevision().value() == 7);

    auto failingPlan   = std::make_shared<FakeSceneRenderPlan>(9, true);
    auto failingSource = FakeRenderPlanSource(failingPlan);
    auto failingResult = controller.bind(goodTarget, failingSource);
    assert(! failingResult);
    assert(failingPlan->prepared);
    assert(controller.boundRenderPlanRevision().has_value());
    assert(controller.boundRenderPlanRevision().value() == 7);
    assert(controller.target().binding == goodTarget.binding);

    wallpaper::OutputController unstableController;
    auto unstablePlanA = std::make_shared<FakeSceneRenderPlan>(13);
    auto unstablePlanB = std::make_shared<FakeSceneRenderPlan>(17);
    auto unstableSource = SequentialRenderPlanSource(unstablePlanA, unstablePlanB);
    auto unstableResult = unstableController.bind(goodTarget, unstableSource);
    assert(unstableResult);
    assert(unstablePlanA->bindCalls == 1);
    assert(unstablePlanB->bindCalls == 0);
    assert(unstableController.boundRenderPlanRevision().has_value());
    assert(unstableController.boundRenderPlanRevision().value() == 13);

    std::size_t destructionCount = 0;
    wallpaper::OutputController ownedPlanController;
    {
        auto ephemeralSource = EphemeralRenderPlanSource(destructionCount);
        auto ownedResult     = ownedPlanController.bind(goodTarget, ephemeralSource);
        assert(ownedResult);
        assert(! ephemeralSource.lastPlan().expired());
    }
    assert(destructionCount == 0);
    assert(ownedPlanController.boundRenderPlan() != nullptr);
    ownedPlanController = wallpaper::OutputController {};
    assert(destructionCount == 1);

    auto genericPlan   = std::make_shared<SurfaceRenderPlan>();
    auto genericSource = SurfaceRenderPlanSource(genericPlan);
    auto genericTargetBinding = std::make_shared<WrongBinding>();
    wallpaper::OutputTarget genericTarget;
    genericTarget.type    = wallpaper::OutputTargetType::Surface;
    genericTarget.binding = genericTargetBinding;
    auto genericResult    = controller.bind(genericTarget, genericSource);
    assert(genericResult);
    assert(genericPlan->prepared);
    assert(controller.boundRenderPlanRevision().has_value());
    assert(controller.boundRenderPlanRevision().value() == 3);
    return 0;
}
