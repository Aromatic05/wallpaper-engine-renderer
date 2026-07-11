#include "backend/scene/internal/parser/WPParticleParser.hpp"
#include "backend/scene/internal/parser/WPSceneParserTestHooks.hpp"
#include "backend/scene/internal/parser/particle/Animation.hpp"
#include "backend/scene/internal/particle/include/particle/ParticleSystem.h"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "utils/Algorism.h"

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

void TestChildInheritsOnlyLayerOpacityAndTint() {
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
    root_override.alpha      = 0.35f;
    root_override.count      = 7.0f;
    root_override.lifetime   = 2.0f;
    root_override.rate       = 3.0f;
    root_override.speed      = 4.0f;
    root_override.size       = 5.0f;
    root_override.overColorn = true;
    root_override.colorn     = { 0.9f, 0.1f, 0.2f };
    root_override.controlpointOffsets[0] = std::array<float, 3> { 8.0f, 9.0f, 10.0f };

    const auto root_result =
        wallpaper::ResolveParticleSubsystemOverride(root_override, false);
    assert(root_result.count == 7.0f && root_result.lifetime == 2.0f &&
           root_result.rate == 3.0f && root_result.speed == 4.0f && root_result.size == 5.0f);
    assert(root_result.controlpointOffsets[0].has_value());

    const auto child_override =
        wallpaper::ResolveParticleSubsystemOverride(root_override, true);
    assert(child_override.enabled);
    assert(Near(child_override.alpha, root_override.alpha));
    assert(child_override.overColorn);
    assert(child_override.colorn == root_override.colorn);
    assert(child_override.count == 1.0f);
    assert(child_override.lifetime == 1.0f);
    assert(child_override.rate == 1.0f);
    assert(child_override.speed == 1.0f);
    assert(child_override.size == 1.0f);
    assert(! child_override.controlpointOffsets[0].has_value());

    wallpaper::LoadParticleInitializers(*child, child_particle, child_override);
    child->Emitt();

    assert(child->InstanceCount() == 1);
    const auto* instance = child->InstanceAt(0);
    assert(instance != nullptr);
    assert(instance->Particles().size() == 1);
    const auto& particle = instance->Particles().front();

    assert(Near(particle.color.x(), root_override.colorn[0]));
    assert(Near(particle.color.y(), root_override.colorn[1]));
    assert(Near(particle.color.z(), root_override.colorn[2]));
    assert(Near(particle.alpha, root_override.alpha));
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
    child->AddEmitter(WPParticleParser::genParticleEmittOp(MakeBoxEmitter(1.0f, 1)));
    auto* child_ptr = child.get();
    parent->AddChild(std::move(child));

    parent->SetRuntimeRateOverride(4.0f);
    parent->Emitt();

    assert(Near(static_cast<float>(child_ptr->Rate()), 4.0f));
    assert(child_ptr->InstanceCount() == 1);
    assert(child_ptr->InstanceAt(0)->Particles().size() == 1);
    assert(Near(child_ptr->InstanceAt(0)->Particles().front().lifetime, -1.0f));
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
void TestDragForceMatchesAuthoredStrength() {
    assert(std::abs(wallpaper::algorism::DragForce(4.0, 0.25, 2.0) + 2.0) < 1e-9);

    const Eigen::Vector3d velocity { 3.0, 4.0, 0.0 };
    const auto drag = wallpaper::algorism::DragForce(velocity, 0.5, 1.0);
    assert((drag - Eigen::Vector3d { -1.5, -2.0, 0.0 }).norm() < 1e-9);
}

void TestRandomFrameUsesStableSpawnRandom() {
    Particle low;
    low.random = -1.0f;
    assert(wallpaper::ResolveParticleRandomFrameLifetime(low) == 0.0f);

    Particle selected;
    selected.random = 0.375f;
    assert(Near(wallpaper::ResolveParticleRandomFrameLifetime(selected), 0.375f));

    Particle high;
    high.random = 1.0f;
    const float upper = wallpaper::ResolveParticleRandomFrameLifetime(high);
    assert(upper < 1.0f && upper > 0.99f);

    Scene scene;
    scene.paritileSys->gener = std::make_unique<DummyRawGener>();
    scene.PassFrameTime(1.0);

    auto subsystem = MakeSubsystem(scene, ParticleSubSystem::SpawnType::STATIC, 1.0, 8);
    subsystem->AddEmitter(WPParticleParser::genParticleEmittOp(MakeBoxEmitter(1.0f, 8)));
    subsystem->Emitt();

    assert(subsystem->InstanceCount() == 1);
    const auto particles = subsystem->InstanceAt(0)->Particles();
    assert(particles.size() == 8);

    bool differs_from_first = false;
    const float first = particles.front().random;
    for (const auto& particle : particles) {
        assert(particle.random >= 0.0f && particle.random < 1.0f);
        assert(Near(wallpaper::ResolveParticleRandomFrameLifetime(particle), particle.random));
        differs_from_first = differs_from_first || particle.random != first;
    }
    assert(differs_from_first);
}

void TestSimulationRateDoesNotChangeEmitterCadence() {
    Scene scene;
    scene.paritileSys->gener = std::make_unique<DummyRawGener>();
    scene.PassFrameTime(1.0);

    auto slow = MakeSubsystem(scene, ParticleSubSystem::SpawnType::STATIC, 0.5, 16);
    auto fast = MakeSubsystem(scene, ParticleSubSystem::SpawnType::STATIC, 2.0, 16);
    slow->AddEmitter(WPParticleParser::genParticleEmittOp(MakeBoxEmitter(4.0f)));
    fast->AddEmitter(WPParticleParser::genParticleEmittOp(MakeBoxEmitter(4.0f)));

    slow->Emitt();
    fast->Emitt();

    const auto slow_particles = slow->InstanceAt(0)->Particles();
    const auto fast_particles = fast->InstanceAt(0)->Particles();
    assert(slow_particles.size() == 4);
    assert(fast_particles.size() == 4);
    assert(Near(slow_particles.front().lifetime, 0.5f));
    assert(Near(fast_particles.front().lifetime, -1.0f));
}

void TestWorldSpaceGravityUsesOwnerBasis() {
    const nlohmann::json movement = {
        { "name", "movement" },
        { "drag", 0.0f },
        { "gravity", std::array<float, 3> { 0.0f, -10.0f, 0.0f } },
    };
    auto movement_operator = WPParticleParser::genParticleOperatorOp(movement, {});

    std::array<Particle, 1> local_particles {};
    wallpaper::ParticleInfo local_info {
        .particles     = local_particles,
        .controlpoints = {},
        .world_space   = false,
        .time          = 0.0,
        .time_pass     = 1.0,
    };
    movement_operator(local_info);
    assert((local_particles.front().velocity - Eigen::Vector3f { 0.0f, -10.0f, 0.0f }).norm()
           < 0.0001f);

    const Eigen::Matrix3d world_from_local =
        Eigen::AngleAxisd(M_PI / 2.0, Eigen::Vector3d::UnitZ()).toRotationMatrix();
    std::array<Particle, 1> world_particles {};
    wallpaper::ParticleInfo world_info {
        .particles            = world_particles,
        .controlpoints        = {},
        .world_from_local_dir = world_from_local,
        .local_from_world_dir = world_from_local.inverse(),
        .world_space          = true,
        .time                 = 0.0,
        .time_pass            = 1.0,
    };
    movement_operator(world_info);
    assert((world_particles.front().velocity - Eigen::Vector3f { -10.0f, 0.0f, 0.0f }).norm()
           < 0.0001f);

    const Eigen::Vector3d restored_world_velocity =
        world_from_local * world_particles.front().velocity.cast<double>();
    assert((restored_world_velocity - Eigen::Vector3d { 0.0, -10.0, 0.0 }).norm() < 0.0001);
}

} // namespace

int main() {
    TestChildInheritsOnlyLayerOpacityAndTint();
    TestStaticChildOnlyAllocatesOneInstance();
    TestRuntimeSizeOverrideDoesNotCompound();
    TestRuntimeRateOverridePropagatesToChild();
    TestPaddedSpriteSheetUsesContentRegion();
    TestDragForceMatchesAuthoredStrength();
    TestRandomFrameUsesStableSpawnRandom();
    TestSimulationRateDoesNotChangeEmitterCadence();
    TestWorldSpaceGravityUsesOwnerBasis();
    return 0;
}
