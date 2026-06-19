#include "WPParticleRawGener.h"

#include <cstring>
#include <cmath>
#include <Eigen/Dense>
#include <array>
#include <vector>

#include "core/Literals.hpp"
#include "SpecTexs.hpp"
#include "ParticleModify.h"
#include "ParticleSystem.h"

#include "utils/Logging.h"

using namespace wallpaper;
using namespace Eigen;

struct WPGOption {
    bool thick_format { false };
    bool geometry_shader { false };
};

namespace
{
inline void AssignVertexTimes(std::span<float> dst, std::span<const float> src, uint num) noexcept {
    const uint dst_one_size = dst.size() / num;
    for (uint i = 0; i < num; i++) {
        std::copy(src.begin(), src.end(), dst.begin() + i * dst_one_size);
    }
}

inline void AssignVertex(std::span<float> dst, std::span<const float> src, uint num) noexcept {
    const uint dst_one_size = dst.size() / num;
    const uint src_one_size = src.size() / num;
    for (uint i = 0; i < num; i++) {
        std::copy_n(src.begin() + i * src_one_size, src_one_size, dst.begin() + i * dst_one_size);
    }
}

inline Vector3f CatmullRom(const Vector3f& p0, const Vector3f& p1, const Vector3f& p2,
                           const Vector3f& p3, float t) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

inline float LerpFloat(float start, float end, float t) noexcept {
    return start + (end - start) * t;
}

inline std::array<float, 4> LerpColor(const Particle& start, const Particle& end, float t) noexcept {
    return {
        LerpFloat(start.color[0], end.color[0], t),
        LerpFloat(start.color[1], end.color[1], t),
        LerpFloat(start.color[2], end.color[2], t),
        LerpFloat(start.alpha, end.alpha, t),
    };
}

inline usize GenParticleData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                             const ParticleRawGenSpecOp& specOp, WPGOption opt,
                             SceneVertexArray& sv) noexcept {
    std::array<float, 32 * 4> storage;

    float* data = storage.data();

    const auto one_size   = sv.OneSize();
    const auto totle_size = 4 * one_size;
    usize      i { 0 };
    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;

        for (const auto& p : inst->Particles()) {
            if (! ParticleModify::LifetimeOk(p)) {
                continue;
            }

            float lifetime = p.lifetime;
            specOp(p, { &lifetime });

            auto  pos  = inst->GetBoundedData().pos + p.position;
            float size = p.size / 2.0f;

            usize offset = 0;

            // pos
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { pos[0], pos[1], pos[2] }, 4);
            offset += 4;
            // TexCoordVec4
            float      rz = p.rotation[2];
            std::array t { 0.0f, 1.0f, rz, size, 1.0f, 1.0f, rz, size,
                           1.0f, 0.0f, rz, size, 0.0f, 0.0f, rz, size };
            AssignVertex({ data + offset, totle_size }, t, 4);
            offset += 4;

            // color
            AssignVertexTimes({ data + offset, totle_size },
                              std::array { p.color[0], p.color[1], p.color[2], p.alpha },
                              4);
            offset += 4;

            if (opt.thick_format) {
                const auto& render_velocity = ParticleModify::GetRenderVelocity(p);
                // Cherry_Blossoms_2.json uses spritetrail/genericparticle, where this attribute is
                // the visual trail axis. Prefer a mapsequence-supplied render axis when present so
                // random petal facing does not perturb the physics velocity that draws the star.
                AssignVertexTimes(
                    { data + offset, totle_size },
                    std::array { render_velocity[0], render_velocity[1], render_velocity[2], lifetime },
                    4);
                offset += 4;
            }
            // TexCoordC2
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { p.rotation[0], p.rotation[1] }, 4);

            sv.SetVertexs((i++) * 4, { data, totle_size });
        }
    }
    return i;
}

inline size_t GenRopeParticleData(std::span<const Particle>   particles,
                                  const Eigen::Vector3f&      instance_offset,
                                  const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                  SceneVertexArray& sv, size_t base_index) {
    /*
    attribute vec4 a_PositionVec4;
    attribute vec4 a_TexCoordVec4;
    attribute vec4 a_TexCoordVec4C1;

    #if THICKFORMAT
    attribute vec4 a_TexCoordVec4C2;
    attribute vec4 a_TexCoordVec4C3;
    attribute vec2 a_TexCoordC4;
    #else
    attribute vec3 a_TexCoordVec3C2;
    attribute vec2 a_TexCoordC3;
    #endif

    attribute vec4 a_Color;

    #define in_ParticleTrailLength (a_TexCoordVec4.w)
    #define in_ParticleTrailPosition (a_TexCoordVec4C1.w)
    */
    std::array<float, 32 * 4> storage;
    float*                    data = storage.data();

    const auto one_size   = sv.OneSize();
    const auto totle_size = one_size * 4;
    const auto subdivisions =
        std::max(1u,
                 static_cast<uint32_t>(std::lround(std::max(
                     1.0f, sv.GetFloatOption(WE_PRENDER_ROPE_SUBDIVISION)))));

    std::vector<const Particle*> live_particles;
    live_particles.reserve(particles.size());
    for (const auto& particle : particles) {
        if (ParticleModify::LifetimeOk(particle)) {
            live_particles.push_back(&particle);
        }
    }
    if (live_particles.size() < 2) return 0;

    const uint32_t alive_count       = static_cast<uint32_t>(live_particles.size());
    const uint32_t num_segments      = alive_count - 1;
    const uint32_t total_points      = num_segments * subdivisions + 1;
    const uint32_t total_subsegments = total_points - 1;
    const float    trail_length      = static_cast<float>(total_subsegments) + 1.0f;

    std::vector<Vector3f>             spline_positions(total_points, Vector3f::Zero());
    std::vector<float>                spline_sizes(total_points, 0.0f);
    std::vector<std::array<float, 4>> spline_colors(total_points, { 1.0f, 1.0f, 1.0f, 1.0f });

    for (uint32_t i = 0; i < num_segments; i++) {
        const auto& p0_src = i > 0 ? *live_particles[i - 1] : *live_particles[i];
        const auto& p1_src = *live_particles[i];
        const auto& p2_src = *live_particles[i + 1];
        const auto& p3_src = i + 2 < alive_count ? *live_particles[i + 2] : *live_particles[i + 1];

        const Vector3f p0 = instance_offset + p0_src.position;
        const Vector3f p1 = instance_offset + p1_src.position;
        const Vector3f p2 = instance_offset + p2_src.position;
        const Vector3f p3 = instance_offset + p3_src.position;

        for (uint32_t k = 0; k < subdivisions; k++) {
            const float    t   = static_cast<float>(k) / static_cast<float>(subdivisions);
            const uint32_t idx = i * subdivisions + k;
            spline_positions[idx] = CatmullRom(p0, p1, p2, p3, t);
            spline_sizes[idx]     = LerpFloat(p1_src.size / 2.0f, p2_src.size / 2.0f, t);
            spline_colors[idx]    = LerpColor(p1_src, p2_src, t);
        }
    }

    const auto& last_particle = *live_particles.back();
    spline_positions[total_points - 1] = instance_offset + last_particle.position;
    spline_sizes[total_points - 1]     = last_particle.size / 2.0f;
    spline_colors[total_points - 1]    = {
        last_particle.color[0],
        last_particle.color[1],
        last_particle.color[2],
        last_particle.alpha,
    };

    for (uint32_t s = 0; s < total_subsegments; s++) {
        const auto& start_pos   = spline_positions[s];
        const auto& end_pos     = spline_positions[s + 1];
        const auto& prev_pos    = s > 0 ? spline_positions[s - 1] : start_pos;
        const auto& after_pos   = s + 2 < total_points ? spline_positions[s + 2] : end_pos;
        const float size_start  = spline_sizes[s];
        const float size_end    = spline_sizes[s + 1];
        const auto& color_start = spline_colors[s];
        const auto& color_end   = spline_colors[s + 1];

        std::size_t offset = 0;

        AssignVertexTimes({ data + offset, totle_size },
                          std::array { start_pos[0], start_pos[1], start_pos[2], size_start },
                          4);
        offset += 4;
        AssignVertexTimes(
            { data + offset, totle_size },
            std::array { end_pos[0], end_pos[1], end_pos[2], trail_length },
            4);
        offset += 4;
        AssignVertexTimes({ data + offset, totle_size },
                          std::array { prev_pos[0], prev_pos[1], prev_pos[2], static_cast<float>(s) },
                          4);
        offset += 4;

        if (opt.thick_format) {
            AssignVertexTimes(
                { data + offset, totle_size },
                std::array { after_pos[0], after_pos[1], after_pos[2], size_end },
                4);
            offset += 4;
            AssignVertexTimes({ data + offset, totle_size },
                              std::array { color_end[0], color_end[1], color_end[2], color_end[3] },
                              4);
            offset += 4;
            std::array t { 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            AssignVertex({ data + offset, totle_size }, t, 4);
            offset += 4;
        } else {
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { after_pos[0], after_pos[1], after_pos[2] }, 4);
            offset += 4;
            std::array t { 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            AssignVertex({ data + offset, totle_size }, t, 4);
            offset += 4;
        }

        AssignVertexTimes({ data + offset, totle_size },
                          std::array { color_start[0], color_start[1], color_start[2], color_start[3] },
                          4);

        if (! sv.SetVertexs((base_index + s) * 4, { data, totle_size })) {
            return s;
        }
    }

    return total_subsegments;
}

inline void updateIndexArray(uint16_t index, size_t count, SceneIndexArray& iarray) noexcept {
    constexpr size_t single_size = 6;
    const uint16_t   cv          = index * 4;

    std::array<uint16_t, single_size> single;
    // 0 1 3
    // 1 2 3
    single[0] = cv;
    single[1] = cv + 1;
    single[2] = cv + 3;
    single[3] = cv + 1;
    single[4] = cv + 2;
    single[5] = cv + 3;
    // every particle
    for (uint16_t i = index; i < count; i++) {
        iarray.AssignHalf(i * single_size, single);
        for (auto& x : single) x += 4;
    }
}
} // namespace

void WPParticleRawGener::GenGLData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                   SceneMesh& mesh, ParticleRawGenSpecOp& specOp) {
    auto& sv = mesh.GetVertexArray(0);
    auto& si = mesh.GetIndexArray(0);

    WPGOption opt;

    opt.thick_format = sv.GetOption(WE_CB_THICK_FORMAT);

    usize particle_num { 0 };

    if (sv.GetOption(WE_PRENDER_ROPE)) {
        for (const auto& inst : instances) {
            if (inst->IsNoLiveParticle()) continue;
            particle_num += GenRopeParticleData(inst->Particles(),
                                                inst->GetBoundedData().pos,
                                                specOp,
                                                opt,
                                                sv,
                                                particle_num);
        }
    } else {
        particle_num += GenParticleData(instances, specOp, opt, sv);
    }

    // LOG_INFO("num: %d", particle_num);

    u16 indexNum = (si.DataCount() * 2) / 6;
    if (particle_num > indexNum) {
        updateIndexArray(indexNum, particle_num, si);
    }
    si.SetRenderDataCount(particle_num * 6 / 2);
}
