#include "api/scene/WESceneOutput.hpp"
#include "output/OutputController.hpp"
#include "output/RenderPlanSource.hpp"

#include <cassert>
#include <memory>

namespace
{
class FakeSceneRenderPlan final : public wallpaper::RenderPlan {
public:
    wallpaper::OutputTargetBindingKind requiredBindingKind() const override {
        return wallpaper::OutputTargetBindingKind::WESceneVulkan;
    }

    std::uint64_t revision() const override { return 7; }

    wallpaper::Result<void> bindOutput(const wallpaper::OutputTarget& target) override {
        auto binding = std::dynamic_pointer_cast<wallpaper::WESceneOutputBinding>(target.binding);
        assert(binding);
        prepared = true;
        return wallpaper::Result<void>::success();
    }

    bool prepared { false };
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

class WrongBinding final : public wallpaper::OutputTargetBinding {
public:
    wallpaper::OutputTargetBindingKind kind() const override {
        return wallpaper::OutputTargetBindingKind::Surface;
    }
};
} // namespace

int main() {
    wallpaper::BackendCapabilities capabilities;
    capabilities.supportsRenderPlan = true;

    auto plan   = std::make_shared<FakeSceneRenderPlan>();
    auto source = FakeRenderPlanSource(plan);

    wallpaper::OutputController controller;

    wallpaper::OutputTarget wrongTarget;
    wrongTarget.type    = wallpaper::OutputTargetType::Surface;
    wrongTarget.binding = std::make_shared<WrongBinding>();
    auto wrongResult    = controller.bind(wrongTarget, source, capabilities);
    assert(! wrongResult);
    assert(! plan->prepared);

    wallpaper::RenderInitInfo initInfo;
    auto                      sceneBinding = wallpaper::MakeWESceneOutputBinding(initInfo);
    auto                      goodTarget   = wallpaper::MakeWESceneOutputTarget(sceneBinding);
    auto                      goodResult   = controller.bind(goodTarget, source, capabilities);
    assert(goodResult);
    assert(plan->prepared);
    return 0;
}
