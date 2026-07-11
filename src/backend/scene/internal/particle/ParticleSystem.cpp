#include "ParticleSystem.h"
#include "core/Literals.hpp"
#include "scene/Scene.h"
#include "scene/SceneNode.h"
#include "ParticleModify.h"
#include "scene/SceneMesh.h"
#include "core/Random.hpp"

#include "utils/Logging.h"

#include <algorithm>
#include <cmath>

using namespace wallpaper;

namespace
{
struct ParticleSystemStats {
    size_t subsystem_count { 0 };
    size_t instance_count { 0 };
    size_t particle_count { 0 };
};

constexpr float kRuntimeSizeEpsilon = 0.000001f;
constexpr double kControlPointTransformDeterminantEpsilon = 0.000000001;

float SafeRuntimeSizeReference(float size) {
    return std::abs(size) > kRuntimeSizeEpsilon ? size : 1.0f;
}

Eigen::Vector3d TransformPoint(const Eigen::Matrix4d& transform, const Eigen::Vector3d& point) {
    const Eigen::Vector4d transformed = transform * Eigen::Vector4d { point.x(), point.y(), point.z(), 1.0 };
    if (std::abs(transformed.w()) <= kControlPointTransformDeterminantEpsilon) {
        // Homogeneous scene transforms should keep w near one. Falling back to the un-divided xyz
        // vector prevents a malformed matrix from turning the linked control point into infinity.
        return transformed.head<3>();
    }
    return transformed.head<3>() / transformed.w();
}
} // namespace

void ParticleInstance::Refresh() {
    SetDeath(false);
    SetNoLiveParticle(false);
    GetBoundedData() = {};
    ParticlesVec().clear();
}

bool ParticleInstance::IsDeath() const { return m_is_death; }
void ParticleInstance::SetDeath(bool v) { m_is_death = v; };

bool ParticleInstance::IsNoLiveParticle() const { return m_no_live_particle; };
void ParticleInstance::SetNoLiveParticle(bool v) { m_no_live_particle = v; };

std::span<const Particle> ParticleInstance::Particles() const { return m_particles; };
std::vector<Particle>&    ParticleInstance::ParticlesVec() { return m_particles; };

ParticleInstance::BoundedData& ParticleInstance::GetBoundedData() { return m_bounded_data; }
const ParticleInstance::BoundedData& ParticleInstance::GetBoundedData() const {
    return m_bounded_data;
}

ParticleSubSystem::ParticleSubSystem(ParticleSystem& p, std::shared_ptr<SceneMesh> sm,
                                     uint32_t maxcount, double rate, u32 maxcount_instance,
                                     double probability, SpawnType type,
                                     ParticleRawGenSpecOp specOp)
    : m_sys(p),
      m_mesh(sm),
      m_maxcount(maxcount),
      m_rate(rate),
      m_genSpecOp(specOp),
      m_time(0),
      m_maxcount_instance(maxcount_instance),
      m_probability(probability),
      m_spawn_type(type) {};

ParticleSubSystem::~ParticleSubSystem() = default;

void ParticleSubSystem::AddEmitter(ParticleEmittOp&& em) { m_emiters.emplace_back(em); }

void ParticleSubSystem::AddInitializer(ParticleInitOp&& ini) { m_initializers.emplace_back(ini); }

void ParticleSubSystem::AddOperator(ParticleOperatorOp&& op) { m_operators.emplace_back(op); }

std::span<const ParticleControlpoint> ParticleSubSystem::Controlpoints() const {
    return m_controlpoints;
}
std::span<ParticleControlpoint> ParticleSubSystem::Controlpoints() { return m_controlpoints; };

ParticleSubSystem::SpawnType ParticleSubSystem::Type() const { return m_spawn_type; }

u32 ParticleSubSystem::MaxInstanceCount() const { return m_maxcount_instance; };

void ParticleSubSystem::SetSceneNode(SceneNode* node) { m_node = node; }

void ParticleSubSystem::ApplyRuntimeColorOverrideToParticle(Particle& particle) const {
    if (!m_runtime_color_override.has_value()) return;

    const auto& color = *m_runtime_color_override;
    const Eigen::Vector3f particle_color { color[0], color[1], color[2] };
    particle.init.color = particle_color;
    particle.color      = particle_color;
}

void ParticleSubSystem::ApplyRuntimeColorOverrideToInstances() {
    for (auto& instance : m_instances) {
        if (!instance) continue;
        for (auto& particle : instance->ParticlesVec()) {
            ApplyRuntimeColorOverrideToParticle(particle);
        }
    }
}

void ParticleSubSystem::SetRuntimeColorOverride(const std::array<float, 3>& color) {
    m_runtime_color_override = color;
    // Live user-property color edits must affect particles that have already been emitted. This is
    // intentionally scoped to the layer's own particle subsystem: nested child particle assets keep
    // their authored initializers, including colorrandom. Wallpaper Engine's composition layers can
    // then color-grade the combined result with their parent effects instead of receiving a flat
    // white child trail from a root-layer colorn override.
    ApplyRuntimeColorOverrideToInstances();
    if (m_mesh) m_mesh->SetDirty();
}

std::optional<std::array<float, 3>> ParticleSubSystem::RuntimeColorOverride() const {
    return m_runtime_color_override;
}

void ParticleSubSystem::SetRuntimeRateOverride(float rate) {
    if (!std::isfinite(rate)) return;

    // Wallpaper treats particle override rate as a non-negative simulation clock multiplier. The
    // same layer-level override is parsed into child emitters, so propagate live script/user edits
    // recursively to keep event-follow trails and their parent particles in the same time domain.
    const float normalized_rate = std::max(0.0f, rate);
    m_rate = normalized_rate;
    m_runtime_rate_override = normalized_rate;

    for (auto& child : m_children) {
        if (child) child->SetRuntimeRateOverride(normalized_rate);
    }
}

std::optional<float> ParticleSubSystem::RuntimeRateOverride() const {
    return m_runtime_rate_override;
}

void ParticleSubSystem::ApplyRuntimeSizeDeltaToParticle(Particle& particle,
                                                        float     size_delta) const {
    particle.init.size *= size_delta;
    particle.size *= size_delta;
}

void ParticleSubSystem::ApplyRuntimeSizeDeltaToInstances(float size_delta) {
    for (auto& instance : m_instances) {
        if (!instance) continue;
        for (auto& particle : instance->ParticlesVec()) {
            ApplyRuntimeSizeDeltaToParticle(particle, size_delta);
        }
    }
}

void ParticleSubSystem::ApplyRuntimeSizeOverrideToNewParticle(Particle& particle) const {
    if (!m_runtime_size_override.has_value()) return;
    if (std::abs(m_runtime_size_ratio - 1.0f) <= kRuntimeSizeEpsilon) return;

    // Newly emitted particles have just received the parse-time instanceoverride.size multiplier
    // from the initializer list. Apply the current live ratio exactly once before the particle is
    // marked old so future Reset() calls preserve the corrected initializer size.
    ApplyRuntimeSizeDeltaToParticle(particle, m_runtime_size_ratio);
}

void ParticleSubSystem::SetRuntimeSizeReference(float size) {
    m_runtime_size_reference = SafeRuntimeSizeReference(size);
    m_runtime_size_override  = size;
    m_runtime_size_ratio     = 1.0f;

    for (auto& child : m_children) {
        if (child) child->SetRuntimeSizeReference(size);
    }
}

void ParticleSubSystem::SetRuntimeSizeOverride(float size) {
    if (!m_runtime_size_reference.has_value()) {
        // Dynamic or legacy parse paths should seed the reference during ParseParticleObj(), but
        // falling back to the first runtime value keeps the subsystem stable if an older caller
        // wires size edits before the parser had a chance to record the cold multiplier.
        m_runtime_size_reference = SafeRuntimeSizeReference(size);
    }

    const float reference = SafeRuntimeSizeReference(*m_runtime_size_reference);
    const float next_ratio = size / reference;
    const float current_ratio =
        std::abs(m_runtime_size_ratio) > kRuntimeSizeEpsilon ? m_runtime_size_ratio : 1.0f;
    const float size_delta = next_ratio / current_ratio;

    m_runtime_size_override = size;
    m_runtime_size_ratio    = next_ratio;

    if (std::isfinite(size_delta) && std::abs(size_delta - 1.0f) > kRuntimeSizeEpsilon) {
        // Existing particles already contain the previous live multiplier in init.size. Scaling by
        // the ratio delta changes them to the new multiplier without rebuilding the emitter or
        // compounding the size on every frame.
        ApplyRuntimeSizeDeltaToInstances(size_delta);
        if (m_mesh) m_mesh->SetDirty();
    }

    for (auto& child : m_children) {
        if (child) child->SetRuntimeSizeOverride(size);
    }
}

std::optional<float> ParticleSubSystem::RuntimeSizeOverride() const {
    return m_runtime_size_override;
}

void ParticleSubSystem::UpdateLinkedControlpoints() {
    bool has_linked_controlpoint = std::any_of(m_controlpoints.begin(), m_controlpoints.end(),
                                               [](const ParticleControlpoint& controlpoint) {
                                                   return controlpoint.link_mouse;
                                               });
    if (!has_linked_controlpoint) return;

    Eigen::Vector3d mouse_scene = m_sys.MouseScenePosition();
    Eigen::Matrix4d scene_to_particle_local = Eigen::Matrix4d::Identity();
    if (m_node != nullptr) {
        m_node->UpdateTrans();
        const auto model = m_node->ModelTrans();
        const double determinant = model.determinant();
        if (!std::isfinite(determinant) ||
            std::abs(determinant) <= kControlPointTransformDeterminantEpsilon) {
            // A linked control point must live in the same local coordinate space as particle
            // positions. If the node transform cannot be inverted, keep the previous offset instead
            // of applying forces from a nonsense location.
            LOG_ERROR("ParticleControlPoint: non-invertible node transform for linked mouse control point");
            return;
        }
        scene_to_particle_local = model.inverse();
    }

    for (auto& controlpoint : m_controlpoints) {
        if (!controlpoint.link_mouse) continue;
        if (controlpoint.worldspace) {
            // World-space control point offsets are authored in scene coordinates. Convert the final
            // world target back into particle-local space because emitters/operators compare against
            // Particle::position, which is generated before the SceneNode model transform is applied.
            controlpoint.offset =
                TransformPoint(scene_to_particle_local, mouse_scene + controlpoint.base_offset);
        } else {
            // Pointer-locked local control points still need the pointer converted through the full
            // inverse node transform. A plain mouse-origin subtraction misses layer scale and
            // rotation, which made scaled particle systems (for example fireflies) ignore the force
            // distance threshold.
            controlpoint.offset =
                controlpoint.base_offset + TransformPoint(scene_to_particle_local, mouse_scene);
        }
    }
}

Eigen::Vector3f ParticleSubSystem::ResolveEventAnchorPosition(
    const Eigen::Vector3f& parent_position) {
    if (m_node == nullptr) return parent_position;

    const Eigen::Matrix4d local_transform = m_node->GetLocalTrans();
    const Eigen::Matrix3d local_linear    = local_transform.block<3, 3>(0, 0);
    const double          determinant     = local_linear.determinant();
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= kControlPointTransformDeterminantEpsilon) {
        if (!m_logged_event_anchor_transform_error) {
            // Event-spawned children are anchored at a parent particle position, then their own
            // child transform is applied by the scene node. The anchor must therefore be expressed
            // in the inverse child basis; if that basis is singular, keep the old raw anchor and
            // log once so the broken authored transform can be diagnosed without flooding frames.
            LOG_ERROR("ParticleEventAnchor: non-invertible child transform for event particle");
            m_logged_event_anchor_transform_error = true;
        }
        return parent_position;
    }

    // Wallpaper's event child transform is applied around the spawned child system, not around the
    // parent particle that triggered it. Convert only the followed parent coordinate through the
    // inverse child linear basis so child scale/rotation enlarge and rotate the glow/trail itself
    // without pushing the glow away from the firefly center. The child translation remains authored
    // as the event offset and is intentionally left out of this inverse.
    return (local_linear.inverse() * parent_position.cast<double>()).cast<float>();
}

void ParticleSubSystem::AddChild(std::unique_ptr<ParticleSubSystem>&& child) {
    m_children.emplace_back(std::move(child));
}

void ParticleSubSystem::CollectStats(size_t* subsystem_count,
                                     size_t* instance_count,
                                     size_t* particle_count) const {
    if (subsystem_count != nullptr) (*subsystem_count)++;

    if (instance_count != nullptr || particle_count != nullptr) {
        for (const auto& instance : m_instances) {
            if (!instance) continue;
            if (instance_count != nullptr) (*instance_count)++;
            if (particle_count != nullptr) (*particle_count) += instance->Particles().size();
        }
    }

    for (const auto& child : m_children) {
        if (child) child->CollectStats(subsystem_count, instance_count, particle_count);
    }
}

ParticleInstance* ParticleSubSystem::QueryNewInstance() {
    if (Random::get(0.0, 1.0) <= m_probability) {
        for (auto& inst : m_instances) {
            if (inst->IsDeath() && inst->IsNoLiveParticle()) {
                inst->Refresh();
                return inst.get();
            }
        }
        if (m_instances.size() < m_maxcount_instance) {
            m_instances.emplace_back(std::make_unique<ParticleInstance>());
            return m_instances.back().get();
        }
    }
    return nullptr;
}

void ParticleSubSystem::Emitt() {
    const double frameTime      = m_sys.scene.frameTime;
    const double simulationTime = frameTime * m_rate;
    m_time += simulationTime;

    UpdateLinkedControlpoints();

    if (m_spawn_type == SpawnType::STATIC) {
        if (m_instances.empty()) m_instances.emplace_back(std::make_unique<ParticleInstance>());
    }

    auto spawn_inst = [](ParticleInstance& inst, ParticleSubSystem& child, isize idx) {
        ParticleInstance* n_inst = child.QueryNewInstance();
        if (n_inst != nullptr) {
            n_inst->GetBoundedData() = {
                .parent       = &inst,
                .particle_idx = idx,
            };
        }
    };

    for (auto& inst : m_instances) {
        assert(inst);

        auto& bounded_data = inst->GetBoundedData();

        bool type_has_death =
            m_spawn_type == SpawnType::EVENT_SPAWN || m_spawn_type == SpawnType::EVENT_FOLLOW;

        // bouded data and death
        if (bounded_data.parent != nullptr) {
            std::span particles = bounded_data.parent->Particles();
            if (bounded_data.particle_idx != -1 && bounded_data.particle_idx < particles.size()) {
                auto& p          = particles[bounded_data.particle_idx];
                bounded_data.pos = ResolveEventAnchorPosition(ParticleModify::GetPos(p));
                // only update pos once when event_death
                if (m_spawn_type == SpawnType::EVENT_DEATH) bounded_data.particle_idx = -1;

                // death if bounded particle death
                if (! inst->IsDeath() && type_has_death) {
                    bool cur_life_ok = ParticleModify::LifetimeOk(p);
                    inst->SetDeath(! cur_life_ok && bounded_data.pre_lifetime_ok);
                    bounded_data.pre_lifetime_ok = cur_life_ok;
                }
            }

            // death if parent death
            if (! inst->IsDeath() && type_has_death) {
                inst->SetDeath(bounded_data.parent->IsDeath());
            }
        }

        // clear when death if follow
        if (inst->IsDeath() && m_spawn_type == SpawnType::EVENT_FOLLOW) {
            inst->ParticlesVec().clear();
        }

        if (! inst->IsDeath()) {
            for (auto& emittOp : m_emiters) {
                emittOp(inst->ParticlesVec(), m_initializers, m_controlpoints, m_maxcount,
                        frameTime);
            }
        }

        // event_death is always death after emitop
        if (m_spawn_type == SpawnType::EVENT_DEATH) inst->SetDeath(true);

        ParticleInfo info {
            .particles     = inst->ParticlesVec(),
            .controlpoints = m_controlpoints,
            .time          = m_time,
            .time_pass     = simulationTime,
        };

        bool  has_live = false;
        isize i        = -1;
        for (auto& p : info.particles) {
            i++;

            if (ParticleModify::IsNew(p)) {
                // new spawn
                for (auto& child : m_children) {
                    if (child->Type() == SpawnType::EVENT_FOLLOW ||
                        child->Type() == SpawnType::EVENT_SPAWN)
                        spawn_inst(*inst, *child, i);
                }
                ApplyRuntimeSizeOverrideToNewParticle(p);
            }

            ParticleModify::MarkOld(p);
            if (! ParticleModify::LifetimeOk(p)) {
                continue;
            }
            ParticleModify::Reset(p);
            ParticleModify::ChangeLifetime(p, -simulationTime);
            // Reset() restores particle color from its initializer snapshot every frame. Re-apply
            // the live instanceoverride color here so user-property color edits survive that reset
            // and run before later particle operators mutate the rendered state.
            ApplyRuntimeColorOverrideToParticle(p);

            if (! ParticleModify::LifetimeOk(p)) {
                // new dead
                for (auto& child : m_children) {
                    if (child->Type() == SpawnType::EVENT_DEATH) spawn_inst(*inst, *child, i);
                }
            } else {
                has_live = true;
            }
        }

        inst->SetNoLiveParticle(! has_live);

        std::for_each(m_operators.begin(), m_operators.end(), [&info](ParticleOperatorOp& op) {
            op(info);
        });
    }

    m_mesh->SetDirty();

    m_sys.gener->GenGLData(m_instances, *m_mesh, m_genSpecOp);

    for (auto& child : m_children) {
        child->Emitt();
    }
}

void ParticleSystem::Emitt() {
    for (auto& el : subsystems) {
        el->Emitt();
    }
}

void ParticleSystem::SetMousePos(float x, float y) { m_mouse_pos = { x, y }; }

std::array<float, 2> ParticleSystem::MousePos() const { return m_mouse_pos; }

Eigen::Vector3d ParticleSystem::MouseScenePosition() const {
    const SceneCamera* camera = scene.activeCamera;
    auto global_camera_it = scene.cameras.find("global");
    if (global_camera_it != scene.cameras.end() && global_camera_it->second) {
        camera = global_camera_it->second.get();
    }

    if (camera == nullptr) {
        return Eigen::Vector3d { m_mouse_pos[0] * scene.ortho[0],
                                 (1.0f - m_mouse_pos[1]) * scene.ortho[1],
                                 0.0 };
    }

    Eigen::Vector3d camera_pos = camera->GetPosition();
    double          left = camera_pos.x() - camera->Width() / 2.0;
    double          top  = camera_pos.y() + camera->Height() / 2.0;
    return Eigen::Vector3d { left + m_mouse_pos[0] * camera->Width(),
                             top - m_mouse_pos[1] * camera->Height(),
                             0.0 };
}
ParticleSystem::~ParticleSystem() = default;
