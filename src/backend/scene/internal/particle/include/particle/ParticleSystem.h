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
                      ParticleRawGenSpecOp specOp, bool world_space = false);
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
    double    Rate() const { return m_rate; }
    usize     InstanceCount() const { return m_instances.size(); }
    const ParticleInstance* InstanceAt(usize index) const {
        return index < m_instances.size() ? m_instances[index].get() : nullptr;
    }
    void      SetSceneNode(SceneNode* node);
    void      SetRuntimeColorOverride(const std::array<float, 3>& color);
    std::optional<std::array<float, 3>> RuntimeColorOverride() const;
    void      SetRuntimeRateOverride(float rate);
    std::optional<float> RuntimeRateOverride() const;
    void      SetRuntimeSizeReference(float size);
    void      SetRuntimeSizeOverride(float size);
    std::optional<float> RuntimeSizeOverride() const;
    void      CollectStats(size_t* subsystem_count,
                           size_t* instance_count,
                           size_t* particle_count) const;

private:
    void UpdateLinkedControlpoints();
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
    // Wallpaper particle `instanceoverride.rate` scales the subsystem simulation clock, not just
    // the spawn count. Keep it mutable so audio/user scripts nested under instanceoverride can
    // speed up gravity, lifetime decay, and emitter timers after the particle system was parsed.
    double               m_rate;
    // Keep the live rate override separate from the parsed particle clock so script init can
    // distinguish "no runtime value has been applied yet" from a parser fallback. Audio-reactive
    // rate scripts commonly capture init(value) as their base multiplier; returning parsed m_rate
    // here would seed them with the already-reduced cold value and shrink every update twice.
    std::optional<float> m_runtime_rate_override;
    double               m_time;

    std::vector<std::unique_ptr<ParticleSubSystem>> m_children;
    std::vector<std::unique_ptr<ParticleInstance>>  m_instances;

    u32       m_maxcount_instance { 1 };
    double    m_probability { 1.0f };
    SpawnType m_spawn_type { SpawnType::STATIC };
    bool      m_world_space { false };
    SceneNode* m_node { nullptr };
    bool       m_logged_event_anchor_transform_error { false };
    // Runtime particle color edits arrive after the initializer list has already produced live
    // particles. Store the normalized replacement color separately so both future spawns and already
    // alive particles can be synchronized without rebuilding the whole particle subsystem.
    std::optional<std::array<float, 3>> m_runtime_color_override;
    // Wallpaper particle `instanceoverride.size` is a multiplier baked into each particle's
    // initializer state. Remember the parse-time multiplier as a reference so live edits can scale
    // existing particles by the precise ratio instead of treating the property as an absolute pixel
    // size or compounding the multiplier every frame.
    std::optional<float> m_runtime_size_reference;
    std::optional<float> m_runtime_size_override;
    float                m_runtime_size_ratio { 1.0f };
};

class Scene;
class ParticleSystem : NoCopy, NoMove {
public:
    ParticleSystem(Scene& scene): scene(scene) {};
    ~ParticleSystem();

    void Emitt();
    void SetMousePos(float x, float y);
    std::array<float, 2> MousePos() const;
    Eigen::Vector3d MouseScenePosition() const;

    Scene& scene;

    std::vector<std::unique_ptr<ParticleSubSystem>> subsystems;
    std::unique_ptr<IParticleRawGener>              gener;

private:
    std::array<float, 2> m_mouse_pos { 0.5f, 0.5f };
};
} // namespace wallpaper
