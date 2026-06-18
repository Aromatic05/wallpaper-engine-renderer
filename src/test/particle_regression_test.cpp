#include "backend/scene/internal/WPParticleParser.hpp"
#include "backend/scene/internal/WPSceneParserTestHooks.hpp"
#include "backend/scene/internal/particle/include/particle/ParticleSystem.h"
#include "backend/scene/internal/scene/include/scene/Scene.h"

#include <cassert>
#include <cmath>
#include <memory>

namespace
{
using wallpaper::Particle;
using wallpaper::ParticleInstance;
using wallpaper::ParticleRawGenSpecOp;
using wallpaper::ParticleSubSystem;
using wallpaper::Scene;
using wallpaper::SceneMesh;
using wallpaper::WPParticleParser;
namespace wpscene = wallpaper::wpscene;

class DummyRawGener final : public wallpaper::IParticleRawGener {
public:
    void GenGLData(std::span<const std::unique_ptr<ParticleInstance>>, SceneMesh&,
                   ParticleRawGenSpecOp&) override {}
};

bool Near(float lhs, float rhs, float epsilon = 0.0001f) {
    return std::abs(lhs - rhs) <= epsilon;
}

std::unique_ptr<ParticleSubSystem> MakeSubsystem(Scene& scene, ParticleSubSystem::SpawnType type,
                                                 double rate = 1.0, uint32_t maxcount = 8,
                                                 uint32_t max_instances = 1) {
    return std::make_unique<ParticleSubSystem>(
        *scene.paritileSys,
        std::make_shared<SceneMesh>(true),
        maxcount,
        rate,
        max_instances,
        1.0,
        type,
        [](const Particle&, const wallpaper::ParticleRawGenSpec&) {});
}

wpscene::Emitter MakeBoxEmitter(float rate, uint32_t instantaneous = 0) {
    wpscene::Emitter emitter;
    emitter.name          = "boxrandom";
    emitter.rate          = rate;
    emitter.instantaneous = instantaneous;
    emitter.distancemin   = { 1.0f, 1.0f, 1.0f };
    emitter.distancemax   = { 1.0f, 1.0f, 1.0f };
    emitter.directions    = { 1.0f, 1.0f, 1.0f };
    emitter.speedmin      = 0.0f;
    emitter.speedmax      = 0.0f;
    return emitter;
}

void TestChildColorRandomSurvivesRootColorOverride() {
    Scene scene;
    scene.paritileSys->gener = std::make_unique<DummyRawGener>();
    scene.PassFrameTime(1.0);

    auto child = MakeSubsystem(scene, ParticleSubSystem::SpawnType::STATIC);
    child->AddEmitter(WPParticleParser::genParticleEmittOp(MakeBoxEmitter(1.0f, 1)));

    wpscene::Particle child_particle;
    child_particle.initializers.push_back(
        nlohmann::json { { "name", "colorrandom" },
                         { "min", "64 128 192" },
                         { "max", "64 128 192" } });

    wpscene::ParticleInstanceoverride root_override;
    root_override.enabled    = true;
    root_override.overColorn = true;
    root_override.colorn     = { 0.9f, 0.1f, 0.2f };

    const auto child_override =
        wallpaper::ResolveParticleSubsystemOverride(root_override, true);
    assert(! child_override.overColor);
    assert(! child_override.overColorn);

    wallpaper::LoadParticleInitializers(*child, child_particle, child_override);
    child->Emitt();

    assert(child->InstanceCount() == 1);
    const auto* instance = child->InstanceAt(0);
    assert(instance != nullptr);
    assert(instance->Particles().size() == 1);
    const auto& particle = instance->Particles().front();

    assert(Near(particle.color.x(), 64.0f / 255.0f));
    assert(Near(particle.color.y(), 128.0f / 255.0f));
    assert(Near(particle.color.z(), 192.0f / 255.0f));
    assert(! Near(particle.color.x(), root_override.colorn[0]));
}

void TestStaticChildOnlyAllocatesOneInstance() {
    Scene scene;
    scene.paritileSys->gener = std::make_unique<DummyRawGener>();
    scene.PassFrameTime(1.0);

    auto parent = MakeSubsystem(scene, ParticleSubSystem::SpawnType::STATIC);
    auto child  = MakeSubsystem(scene, ParticleSubSystem::SpawnType::STATIC, 1.0, 4, 4);
    child->AddEmitter(WPParticleParser::genParticleEmittOp(MakeBoxEmitter(1.0f, 1)));
    auto* child_ptr = child.get();
    parent->AddChild(std::move(child));

    parent->Emitt();
    parent->Emitt();
    parent->Emitt();

    assert(child_ptr->InstanceCount() == 1);
}

void TestRuntimeSizeOverrideDoesNotCompound() {
    Scene scene;
    scene.paritileSys->gener = std::make_unique<DummyRawGener>();
    scene.PassFrameTime(1.0);

    auto subsystem = MakeSubsystem(scene, ParticleSubSystem::SpawnType::STATIC);
    subsystem->AddEmitter(WPParticleParser::genParticleEmittOp(MakeBoxEmitter(1.0f, 1)));

    wpscene::Particle particle_def;
    particle_def.initializers.push_back(
        nlohmann::json { { "name", "sizerandom" }, { "min", 10.0f }, { "max", 10.0f } });
    wallpaper::LoadParticleInitializers(*subsystem, particle_def, {});

    subsystem->Emitt();
    const auto* instance = subsystem->InstanceAt(0);
    assert(instance != nullptr);
    assert(instance->Particles().size() == 1);
    assert(Near(instance->Particles().front().size, 10.0f));

    subsystem->SetRuntimeSizeReference(1.0f);
    subsystem->SetRuntimeSizeOverride(2.0f);
    assert(Near(subsystem->InstanceAt(0)->Particles().front().size, 20.0f));

    subsystem->SetRuntimeSizeOverride(3.0f);
    assert(Near(subsystem->InstanceAt(0)->Particles().front().size, 30.0f));
}

void TestRuntimeRateOverridePropagatesToChild() {
    Scene scene;
    scene.paritileSys->gener = std::make_unique<DummyRawGener>();
    scene.PassFrameTime(0.5);

    auto parent = MakeSubsystem(scene, ParticleSubSystem::SpawnType::STATIC);
    auto child  = MakeSubsystem(scene, ParticleSubSystem::SpawnType::STATIC, 1.0, 8, 1);
    child->AddEmitter(WPParticleParser::genParticleEmittOp(MakeBoxEmitter(1.0f)));
    auto* child_ptr = child.get();
    parent->AddChild(std::move(child));

    parent->SetRuntimeRateOverride(4.0f);
    parent->Emitt();

    assert(Near(static_cast<float>(child_ptr->Rate()), 4.0f));
    assert(child_ptr->InstanceCount() == 1);
    assert(child_ptr->InstanceAt(0)->Particles().size() == 2);
}

void TestPaddedSpriteSheetUsesContentRegion() {
    wallpaper::ImageHeader header;
    header.width     = 8;
    header.height    = 8;
    header.mapWidth  = 7;
    header.mapHeight = 7;
    header.isSprite  = true;

    wallpaper::SpriteFrame frame;
    frame.width  = 4.0f;
    frame.height = 4.0f;

    const auto resolution = wallpaper::ResolvePaddedSpriteSheetResolution(header, frame);
    assert(resolution[0] == 8);
    assert(resolution[1] == 8);
    assert(resolution[2] == 4);
    assert(resolution[3] == 4);
}
} // namespace

int main() {
    TestChildColorRandomSurvivesRootColorOverride();
    TestStaticChildOnlyAllocatesOneInstance();
    TestRuntimeSizeOverrideDoesNotCompound();
    TestRuntimeRateOverridePropagatesToChild();
    TestPaddedSpriteSheetUsesContentRegion();
    return 0;
}
