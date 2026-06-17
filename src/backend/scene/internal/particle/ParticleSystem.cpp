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
constexpr float kRuntimeSizeEpsilon = 0.000001f;
constexpr double kTransformDeterminantEpsilon = 0.000000001;

float SafeRuntimeSizeReference(float size) {
    return std::abs(size) > kRuntimeSizeEpsilon ? size : 1.0f;
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
      m_spawn_type(type) {}

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
    ApplyRuntimeColorOverrideToInstances();
    if (m_mesh) m_mesh->SetDirty();
}

void ParticleSubSystem::SetRuntimeRateOverride(float rate) {
    if (!std::isfinite(rate)) return;

    m_rate = std::max(0.0, static_cast<double>(rate));
    for (auto& child : m_children) {
        if (child) child->SetRuntimeRateOverride(rate);
    }
}

void ParticleSubSystem::ApplyRuntimeSizeDeltaToParticle(Particle& particle, float size_delta) const {
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
    if (!m_runtime_size_reference.has_value()) return;
    if (std::abs(m_runtime_size_ratio - 1.0f) <= kRuntimeSizeEpsilon) return;

    ApplyRuntimeSizeDeltaToParticle(particle, m_runtime_size_ratio);
}

void ParticleSubSystem::SetRuntimeSizeReference(float size) {
    m_runtime_size_reference = SafeRuntimeSizeReference(size);
    m_runtime_size_ratio = 1.0f;
    for (auto& child : m_children) {
        if (child) child->SetRuntimeSizeReference(size);
    }
}

void ParticleSubSystem::SetRuntimeSizeOverride(float size) {
    if (!m_runtime_size_reference.has_value()) {
        m_runtime_size_reference = SafeRuntimeSizeReference(size);
    }

    const float reference = SafeRuntimeSizeReference(*m_runtime_size_reference);
    const float next_ratio = size / reference;
    const float current_ratio =
        std::abs(m_runtime_size_ratio) > kRuntimeSizeEpsilon ? m_runtime_size_ratio : 1.0f;
    const float size_delta = next_ratio / current_ratio;

    m_runtime_size_ratio = next_ratio;

    if (std::isfinite(size_delta) && std::abs(size_delta - 1.0f) > kRuntimeSizeEpsilon) {
        ApplyRuntimeSizeDeltaToInstances(size_delta);
        if (m_mesh) m_mesh->SetDirty();
    }

    for (auto& child : m_children) {
        if (child) child->SetRuntimeSizeOverride(size);
    }
}

Eigen::Vector3f ParticleSubSystem::ResolveEventAnchorPosition(const Eigen::Vector3f& parent_position) {
    if (m_node == nullptr) return parent_position;

    const Eigen::Matrix4d local_transform = m_node->GetLocalTrans();
    const Eigen::Matrix3d local_linear = local_transform.block<3, 3>(0, 0);
    const double determinant = local_linear.determinant();
    if (!std::isfinite(determinant) || std::abs(determinant) <= kTransformDeterminantEpsilon) {
        if (!m_logged_event_anchor_transform_error) {
            LOG_ERROR("ParticleEventAnchor: non-invertible child transform for event particle");
            m_logged_event_anchor_transform_error = true;
        }
        return parent_position;
    }

    return (local_linear.inverse() * parent_position.cast<double>()).cast<float>();
}

void ParticleSubSystem::AddChild(std::unique_ptr<ParticleSubSystem>&& child) {
    m_children.emplace_back(std::move(child));
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
    double frameTime    = m_sys.scene.frameTime;
    double particleTime = frameTime * m_rate;
    m_time += particleTime;

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

        const bool type_has_death =
            m_spawn_type == SpawnType::EVENT_SPAWN || m_spawn_type == SpawnType::EVENT_FOLLOW;

        if (bounded_data.parent != nullptr) {
            std::span particles = bounded_data.parent->Particles();
            if (bounded_data.particle_idx != -1 && bounded_data.particle_idx < particles.size()) {
                auto& p          = particles[bounded_data.particle_idx];
                bounded_data.pos = ResolveEventAnchorPosition(ParticleModify::GetPos(p));
                if (m_spawn_type == SpawnType::EVENT_DEATH) bounded_data.particle_idx = -1;

                if (!inst->IsDeath() && type_has_death) {
                    const bool cur_life_ok = ParticleModify::LifetimeOk(p);
                    inst->SetDeath(!cur_life_ok && bounded_data.pre_lifetime_ok);
                    bounded_data.pre_lifetime_ok = cur_life_ok;
                }
            }

            if (!inst->IsDeath() && type_has_death) {
                inst->SetDeath(bounded_data.parent->IsDeath());
            }
        }

        if (inst->IsDeath() && m_spawn_type == SpawnType::EVENT_FOLLOW) {
            inst->ParticlesVec().clear();
        }

        if (!inst->IsDeath()) {
            for (auto& emittOp : m_emiters) {
                emittOp(inst->ParticlesVec(), m_initializers, m_maxcount, particleTime);
            }
        }

        if (m_spawn_type == SpawnType::EVENT_DEATH) inst->SetDeath(true);

        ParticleInfo info {
            .particles     = inst->ParticlesVec(),
            .controlpoints = m_controlpoints,
            .time          = m_time,
            .time_pass     = particleTime,
        };

        bool  has_live = false;
        isize i        = -1;
        for (auto& p : info.particles) {
            i++;

            if (ParticleModify::IsNew(p)) {
                for (auto& child : m_children) {
                    if (child->Type() == SpawnType::EVENT_FOLLOW ||
                        child->Type() == SpawnType::EVENT_SPAWN) {
                        spawn_inst(*inst, *child, i);
                    }
                }
                ApplyRuntimeSizeOverrideToNewParticle(p);
            }

            ParticleModify::MarkOld(p);
            if (!ParticleModify::LifetimeOk(p)) {
                continue;
            }
            ParticleModify::Reset(p);
            ParticleModify::ChangeLifetime(p, -particleTime);
            ApplyRuntimeColorOverrideToParticle(p);

            if (!ParticleModify::LifetimeOk(p)) {
                for (auto& child : m_children) {
                    if (child->Type() == SpawnType::EVENT_DEATH) spawn_inst(*inst, *child, i);
                }
            } else {
                has_live = true;
            }
        }

        inst->SetNoLiveParticle(!has_live);

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
