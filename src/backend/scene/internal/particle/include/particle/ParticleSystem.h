#pragma once
#include "ParticleEmitter.h"
#include "interface/IParticleRawGener.h"
#include "core/NoCopyMove.hpp"
#include "core/MapSet.hpp"

#include <memory>
#include <optional>

namespace wallpaper
{

class SceneNode;

enum class ParticleAnimationMode
{
    SEQUENCE,
    RANDOMONE,
};

class ParticleSystem;

class ParticleInstance : NoCopy, NoMove {
public:
    struct BoundedData {
        ParticleInstance* parent { nullptr };
        isize             particle_idx { -1 };

        bool            pre_lifetime_ok { true };
        Eigen::Vector3f pos { 0.0f, 0.0f, 0.0f };
    };

    void Refresh();

    bool IsDeath() const;
    void SetDeath(bool);

    bool IsNoLiveParticle() const;
    void SetNoLiveParticle(bool);

    std::span<const Particle> Particles() const;
    std::vector<Particle>&    ParticlesVec();

    BoundedData& GetBoundedData();
    const BoundedData& GetBoundedData() const;

private:
    bool                  m_is_death { false };
    bool                  m_no_live_particle { false };
    std::vector<Particle> m_particles;
    BoundedData           m_bounded_data;
};

class ParticleSubSystem : NoCopy, NoMove {
public:
    enum class SpawnType
    {
        STATIC,
        EVENT_FOLLOW,
        EVENT_SPAWN,
        EVENT_DEATH,
    };

public:
    ParticleSubSystem(ParticleSystem& p, std::shared_ptr<SceneMesh> sm, uint32_t maxcount,
                      double rate, u32 maxcount_instance, double probability, SpawnType type,
                      ParticleRawGenSpecOp specOp);
    ~ParticleSubSystem();

    void Emitt();

    ParticleInstance* QueryNewInstance();

    void AddEmitter(ParticleEmittOp&&);
    void AddInitializer(ParticleInitOp&&);
    void AddOperator(ParticleOperatorOp&&);

    void AddChild(std::unique_ptr<ParticleSubSystem>&&);

    std::span<const ParticleControlpoint> Controlpoints() const;
    std::span<ParticleControlpoint>       Controlpoints();

    SpawnType Type() const;
    u32       MaxInstanceCount() const;
    void      SetSceneNode(SceneNode* node);
    void      SetRuntimeColorOverride(const std::array<float, 3>& color);
    void      SetRuntimeRateOverride(float rate);
    void      SetRuntimeSizeReference(float size);
    void      SetRuntimeSizeOverride(float size);
    std::size_t InstanceCount() const { return m_instances.size(); }
    const ParticleInstance* InstanceAt(std::size_t index) const { return m_instances.at(index).get(); }
    std::size_t ChildCount() const { return m_children.size(); }
    const ParticleSubSystem* ChildAt(std::size_t index) const { return m_children.at(index).get(); }
    double Rate() const { return m_rate; }

private:
    Eigen::Vector3f ResolveEventAnchorPosition(const Eigen::Vector3f& parent_position);
    void ApplyRuntimeColorOverrideToParticle(Particle& particle) const;
    void ApplyRuntimeColorOverrideToInstances();
    void ApplyRuntimeSizeDeltaToParticle(Particle& particle, float size_delta) const;
    void ApplyRuntimeSizeDeltaToInstances(float size_delta);
    void ApplyRuntimeSizeOverrideToNewParticle(Particle& particle) const;

    ParticleSystem&            m_sys;
    std::shared_ptr<SceneMesh> m_mesh;
    //	std::vector<std::unique_ptr<ParticleEmitter>> m_emiters;
    std::vector<ParticleEmittOp> m_emiters;

    // std::vector<Particle>           m_particles;
    std::vector<ParticleInitOp>     m_initializers;
    std::vector<ParticleOperatorOp> m_operators;

    std::array<ParticleControlpoint, 8> m_controlpoints;

    ParticleRawGenSpecOp m_genSpecOp;
    u32                  m_maxcount;
    double               m_rate;
    double               m_time;

    std::vector<std::unique_ptr<ParticleSubSystem>> m_children;
    std::vector<std::unique_ptr<ParticleInstance>>  m_instances;

    u32       m_maxcount_instance { 1 };
    double    m_probability { 1.0f };
    SpawnType m_spawn_type { SpawnType::STATIC };
    SceneNode* m_node { nullptr };
    bool       m_logged_event_anchor_transform_error { false };
    std::optional<std::array<float, 3>> m_runtime_color_override;
    std::optional<float> m_runtime_size_reference;
    float                m_runtime_size_ratio { 1.0f };
};

class Scene;
class ParticleSystem : NoCopy, NoMove {
public:
    ParticleSystem(Scene& scene): scene(scene) {};
    ~ParticleSystem() = default;

    void Emitt();

    Scene& scene;

    std::vector<std::unique_ptr<ParticleSubSystem>> subsystems;
    std::unique_ptr<IParticleRawGener>              gener;
};
} // namespace wallpaper
