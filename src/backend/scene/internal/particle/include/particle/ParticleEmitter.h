#pragma once
#include "Particle.h"

#include <vector>
#include <random>
#include <memory>
#include <functional>
#include <array>
#include <span>
#include <cstdint>

#include "core/Literals.hpp"

namespace wallpaper
{

struct ParticleControlpoint {
    bool            link_mouse { false };
    bool            worldspace { false };
    Eigen::Vector3d offset { 0, 0, 0 };
    Eigen::Vector3d base_offset { 0, 0, 0 };
};

struct ParticleInfo {
    std::span<Particle>                   particles;
    std::span<const ParticleControlpoint> controlpoints;
    double                                time;
    double                                time_pass;
};

struct ParticleInitInfo {
    // The cursor blossom in Cherry_Blossoms_2.json uses mapsequencearoundcontrolpoint, which needs
    // the live mouse-linked control point and a stable 0..4 spawn slot. Keep this context limited to
    // initializers so ordinary operators still receive the existing ParticleInfo path.
    double                                duration { 0.0 };
    std::span<const ParticleControlpoint> controlpoints;
    uint64_t                              sequence { 0 };
};

using ParticleInitOp = std::function<void(Particle&, const ParticleInitInfo&)>;
// particle index lifetime-percent passTime
using ParticleOperatorOp = std::function<void(const ParticleInfo&)>;

using ParticleEmittOp =
    std::function<void(std::vector<Particle>&, std::vector<ParticleInitOp>&,
                       std::span<const ParticleControlpoint>, uint32_t maxcount, double timepass)>;

struct ParticleBoxEmitterArgs {
    std::array<float, 3> directions;
    std::array<float, 3> minDistance;
    std::array<float, 3> maxDistance;
    float                emitSpeed;
    std::array<float, 3> orgin;
    i32                  controlpoint { 0 };
    bool                 one_per_frame;
    bool                 sort;
    u32                  instantaneous;
    float                minSpeed;
    float                maxSpeed;

    static ParticleEmittOp MakeEmittOp(ParticleBoxEmitterArgs);
};

struct ParticleSphereEmitterArgs {
    std::array<float, 3>   directions;
    float                  minDistance;
    float                  maxDistance;
    float                  emitSpeed;
    std::array<float, 3>   orgin;
    i32                    controlpoint { 0 };
    std::array<int32_t, 3> sign;
    bool                   one_per_frame;
    bool                   sort;
    u32                    instantaneous;
    float                  minSpeed;
    float                  maxSpeed;

    static ParticleEmittOp MakeEmittOp(ParticleSphereEmitterArgs);
};

} // namespace wallpaper
