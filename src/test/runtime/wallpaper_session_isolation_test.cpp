#include "api/WallpaperSession.hpp"
#include "output/RenderPlanSource.hpp"
#include "runtime/backend/BackendFactory.hpp"
#include "runtime/backend/ContentBackend.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class IsolationBinding final : public wallpaper::OutputTargetBinding {
public:
    wallpaper::OutputTargetBindingKind kind() const override {
        return wallpaper::OutputTargetBindingKind::Surface;
    }
};

class IsolationPlan final : public wallpaper::RenderPlan {
public:
    explicit IsolationPlan(int owner_id)
        : ownerId(owner_id) {}

    wallpaper::OutputTargetBindingKind requiredBindingKind() const override {
        return wallpaper::OutputTargetBindingKind::Surface;
    }

    std::uint64_t revision() const override { return 1; }

    wallpaper::Result<void> bindOutput(const wallpaper::OutputTarget& target) override {
        assert(target.binding != nullptr);
        assert(target.binding->kind() == wallpaper::OutputTargetBindingKind::Surface);
        ++bindCalls;
        return wallpaper::Result<void>::success();
    }

    int ownerId { 0 };
    int bindCalls { 0 };
};

class IsolationSource final : public wallpaper::RenderPlanSource {
public:
    explicit IsolationSource(int owner_id)
        : plan(std::make_shared<IsolationPlan>(owner_id)) {}

    std::shared_ptr<IsolationPlan> plan;

protected:
    wallpaper::Result<wallpaper::RenderPlanPtr> currentRenderPlan() const override {
        return wallpaper::Result<wallpaper::RenderPlanPtr>::success(plan);
    }
};

class IsolationBackend final : public wallpaper::ContentBackend {
public:
    explicit IsolationBackend(int backend_id)
        : id(backend_id)
        , output(backend_id) {}

    wallpaper::BackendType type() const override { return wallpaper::BackendType::WEScene; }
    wallpaper::BackendCapabilities capabilities() const override { return {}; }

    wallpaper::Result<void> load(const wallpaper::WallpaperSource& source) override {
        loadedSource = source;
        ready = wallpaper::BackendReadyState::Loaded;
        return wallpaper::Result<void>::success();
    }

    wallpaper::Result<void> start() override {
        ++startCalls;
        return wallpaper::Result<void>::success();
    }
    wallpaper::Result<void> pause() override { return wallpaper::Result<void>::success(); }
    wallpaper::Result<void> resume() override { return wallpaper::Result<void>::success(); }
    wallpaper::Result<void> stop() override { return wallpaper::Result<void>::success(); }

    wallpaper::Result<void> setProperty(std::string_view name,
                                        wallpaper::PropertyValue value) override {
        properties[std::string(name)] = std::move(value);
        if (name == "fail") {
            return wallpaper::Result<void>::failure(
                wallpaper::ResultCode::InvalidArgument,
                "backend " + std::to_string(id) + " rejected fail");
        }
        return wallpaper::Result<void>::success();
    }

    wallpaper::Result<void> sendInput(const wallpaper::InputEvent& event) override {
        inputs.push_back(event);
        return wallpaper::Result<void>::success();
    }

    wallpaper::Result<wallpaper::FrameLifecycle> tick() override {
        ++tickCalls;
        wallpaper::FrameLifecycle lifecycle;
        lifecycle.frameRequested = true;
        return wallpaper::Result<wallpaper::FrameLifecycle>::success(lifecycle);
    }

    wallpaper::BackendReadyState readyState() const override { return ready; }
    wallpaper::OutputSource& outputSource() override { return output; }

    wallpaper::DiagnosticsSnapshot diagnostics() const override {
        wallpaper::DiagnosticsSnapshot snapshot;
        snapshot.append(wallpaper::DiagnosticSeverity::Info,
                        "backend." + std::to_string(id),
                        loadedSource.uri);
        return snapshot;
    }

    int id { 0 };
    int startCalls { 0 };
    int tickCalls { 0 };
    wallpaper::BackendReadyState ready { wallpaper::BackendReadyState::Idle };
    wallpaper::WallpaperSource loadedSource { wallpaper::BackendType::WEScene, {}, {} };
    wallpaper::PropertyMap properties;
    std::vector<wallpaper::InputEvent> inputs;
    IsolationSource output;
};

class IsolationFactory final : public wallpaper::BackendFactory {
public:
    wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>> create(
        wallpaper::BackendType type,
        const wallpaper::BackendContext& context) override {
        assert(type == wallpaper::BackendType::WEScene);
        contexts.push_back(context.cachePath);
        auto backend = std::make_unique<IsolationBackend>(nextId++);
        backends.push_back(backend.get());
        return wallpaper::Result<std::unique_ptr<wallpaper::ContentBackend>>::success(
            std::move(backend));
    }

    int nextId { 1 };
    std::vector<std::string> contexts;
    std::vector<IsolationBackend*> backends;
};

bool HasDiagnostic(const wallpaper::DiagnosticsSnapshot& snapshot,
                   std::string_view source,
                   std::string_view message_fragment) {
    for (const auto& entry : snapshot.entries) {
        if (entry.source == source && entry.message.find(message_fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}
} // namespace

int main() {
    auto factory = std::make_shared<IsolationFactory>();

    wallpaper::SessionConfig config_a;
    config_a.backendFactory = factory;
    config_a.cachePath = "/tmp/we-session-isolation-a";
    wallpaper::SessionConfig config_b;
    config_b.backendFactory = factory;
    config_b.cachePath = "/tmp/we-session-isolation-b";

    wallpaper::WallpaperSession session_a(config_a);
    wallpaper::WallpaperSession session_b(config_b);

    assert(session_a.setProperty("sessionTag", std::string("A")));
    assert(session_b.setProperty("sessionTag", std::string("B")));

    wallpaper::WallpaperSource source_a {
        wallpaper::BackendType::WEScene,
        "fake://scene-a",
        { { "sourceOnly", std::string("A-source") } },
    };
    wallpaper::WallpaperSource source_b {
        wallpaper::BackendType::WEScene,
        "fake://scene-b",
        { { "sourceOnly", std::string("B-source") } },
    };
    assert(session_a.load(source_a));
    assert(session_b.load(source_b));
    assert(factory->backends.size() == 2);
    auto* backend_a = factory->backends[0];
    auto* backend_b = factory->backends[1];
    assert(backend_a != backend_b);
    assert(factory->contexts[0] == config_a.cachePath);
    assert(factory->contexts[1] == config_b.cachePath);
    assert(backend_a->loadedSource.uri == "fake://scene-a");
    assert(backend_b->loadedSource.uri == "fake://scene-b");
    assert(std::get<std::string>(backend_a->loadedSource.initialProperties.at("sessionTag")) == "A");
    assert(std::get<std::string>(backend_b->loadedSource.initialProperties.at("sessionTag")) == "B");
    assert(std::get<std::string>(backend_a->loadedSource.initialProperties.at("sourceOnly")) ==
           "A-source");
    assert(std::get<std::string>(backend_b->loadedSource.initialProperties.at("sourceOnly")) ==
           "B-source");

    wallpaper::OutputTarget output_a;
    output_a.type = wallpaper::OutputTargetType::Surface;
    output_a.binding = std::make_shared<IsolationBinding>();
    wallpaper::OutputTarget output_b;
    output_b.type = wallpaper::OutputTargetType::Surface;
    output_b.binding = std::make_shared<IsolationBinding>();
    assert(session_a.bindOutput(output_a));
    assert(session_b.bindOutput(output_b));
    assert(backend_a->output.plan->ownerId == 1);
    assert(backend_b->output.plan->ownerId == 2);
    assert(backend_a->output.plan->bindCalls == 1);
    assert(backend_b->output.plan->bindCalls == 1);

    assert(session_a.setProperty("runtime", std::int32_t(11)));
    assert(session_b.setProperty("runtime", std::int32_t(22)));
    assert(std::get<std::int32_t>(backend_a->properties.at("runtime")) == 11);
    assert(std::get<std::int32_t>(backend_b->properties.at("runtime")) == 22);

    wallpaper::InputEvent input_a;
    input_a.type = wallpaper::InputEventType::PointerMove;
    input_a.pointerX = 0.25;
    input_a.pointerY = 0.75;
    wallpaper::InputEvent input_b;
    input_b.type = wallpaper::InputEventType::KeyDown;
    input_b.keyCode = 65;
    input_b.unicodeChar = 'B';
    assert(session_a.sendInput(input_a));
    assert(session_b.sendInput(input_b));
    assert(backend_a->inputs.size() == 1);
    assert(backend_b->inputs.size() == 1);
    assert(backend_a->inputs[0].type == wallpaper::InputEventType::PointerMove);
    assert(backend_b->inputs[0].type == wallpaper::InputEventType::KeyDown);
    assert(backend_a->inputs[0].pointerX == 0.25);
    assert(backend_b->inputs[0].unicodeChar == 'B');

    assert(session_a.tick());
    assert(backend_a->tickCalls == 1);
    assert(backend_b->tickCalls == 0);
    assert(session_b.tick());
    assert(backend_a->tickCalls == 1);
    assert(backend_b->tickCalls == 1);

    const auto fail = session_a.setProperty("fail", true);
    assert(! fail);
    const auto diagnostics_a = session_a.diagnostics();
    const auto diagnostics_b = session_b.diagnostics();
    assert(HasDiagnostic(diagnostics_a, "runtime.property", "backend 1 rejected fail"));
    assert(HasDiagnostic(diagnostics_a, "backend.1", "fake://scene-a"));
    assert(!HasDiagnostic(diagnostics_a, "backend.2", "fake://scene-b"));
    assert(HasDiagnostic(diagnostics_b, "backend.2", "fake://scene-b"));
    assert(!HasDiagnostic(diagnostics_b, "runtime.property", "backend 1 rejected fail"));
    assert(!HasDiagnostic(diagnostics_b, "backend.1", "fake://scene-a"));
    return 0;
}
