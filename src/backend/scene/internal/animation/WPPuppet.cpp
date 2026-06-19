#include "WPPuppet.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include "utils/Logging.h"

using namespace wallpaper;
using namespace Eigen;

static Quaterniond ToQuaternion(Vector3f euler) {
    const std::array<Vector3d, 3> axis { Vector3d::UnitX(), Vector3d::UnitY(), Vector3d::UnitZ() };
    return AngleAxis<double>(euler.z(), axis[2]) * AngleAxis<double>(euler.y(), axis[1]) *
           AngleAxis<double>(euler.x(), axis[0]);
};

void WPPuppet::prepared() {
    std::vector<Affine3f> combined_tran(bones.size());
    for (uint i = 0; i < bones.size(); i++) {
        auto& b = bones[i];
        if (!b.noParent() && b.parent >= i) {
            LOG_ERROR("puppet bone %u has invalid parent index %u during prepare, fallback to root", i, b.parent);
            b.parent = 0xFFFFFFFFu;
        }
        combined_tran[i] =
            (b.noParent() ? Affine3f::Identity() : combined_tran[b.parent]) * b.transform;

        b.offset_trans = combined_tran[i].inverse();
        /*
        b.world_axis_x = (b.offset_trans.linear() *
        Vector3f::UnitX()).normalized(); b.world_axis_y =
        (b.offset_trans.linear() * Vector3f::UnitY()).normalized();
        b.world_axis_z = (b.offset_trans.linear() *
        Vector3f::UnitZ()).normalized();
        */
    }
    for (auto& anim : anims) {
        anim.frame_time = 1.0f / anim.fps;
        anim.max_time   = anim.length / anim.fps;
        for (auto& b : anim.bframes_array) {
            for (auto& f : b.frames) {
                f.quaternion = ToQuaternion(f.angle);
            }
        }
    }

    m_final_affines.resize(bones.size());
    m_bone_model_affines.resize(bones.size());
}

std::span<const Eigen::Affine3f> WPPuppet::genFrame(WPPuppetLayer& puppet_layer,
                                                    double         time) noexcept {
    double global_blend = puppet_layer.m_global_blend;
    double total_blend = puppet_layer.m_total_blend;

    puppet_layer.updateInterpolation(time);

    for (uint i = 0; i < m_final_affines.size(); i++) {
        const auto& bone   = bones[i];
        auto&       affine = m_final_affines[i];

        affine = Affine3f::Identity();
        const Affine3f parent =
            (bone.noParent() || bone.parent >= i) ? Affine3f::Identity() : m_final_affines[bone.parent];

        Vector3f    trans { bone.transform.translation() * global_blend };
        Vector3f    scale { Vector3f::Ones() * global_blend };
        Quaterniond quat { Quaterniond::Identity() };
        Quaterniond ident { Quaterniond::Identity() };

        // double cur_blend { 0.0f };

        for (auto& layer : puppet_layer.m_layers) {
            auto& alayer = layer.anim_layer;
            if (layer.anim == nullptr || ! alayer.visible) continue;
            assert(i < layer.anim->bframes_array.size());
            if (i >= layer.anim->bframes_array.size()) continue;

            auto&  info    = layer.interp_info;
            auto&  frame_base = layer.anim->bframes_array[i].frames[(usize)0];
            auto&  frame_a = layer.anim->bframes_array[i].frames[(usize)info.frame_a];
            auto&  frame_b = layer.anim->bframes_array[i].frames[(usize)info.frame_b];

            double t = info.t;
            double one_t   = 1.0f - info.t;

            // break up the delta quaternions from the animation start quaternion
            // blend the starting quaternion using the reduced blending factor
            // blend the delta using the full blending factor
            auto frame_a_quat_delta = frame_a.quaternion * frame_base.quaternion.conjugate();
            auto frame_b_quat_delta = frame_b.quaternion * frame_base.quaternion.conjugate();
            quat *= frame_a_quat_delta.slerp(info.t, frame_b_quat_delta).slerp(1.0 - layer.anim_layer.blend, ident) 
                * frame_base.quaternion.slerp(1.0 - (layer.blend), ident);
                       
            // break up the delta positions from the animation start position
            // blend the starting position using the reduced blending factor
            // blend the delta using the full blending factor
            auto frame_a_pos_delta = frame_a.position - frame_base.position;
            auto frame_b_pos_delta = frame_b.position - frame_base.position;
            trans += (layer.blend * frame_base.position) + (layer.anim_layer.blend * (frame_a_pos_delta * one_t + frame_b_pos_delta * t));

            // break up the delta scales from the animation start scale
            // blend the starting scale using the reduced blending factor
            // blend the delta using the full blending factor
            auto& frame_a_scale_delta = frame_a.scale - frame_base.scale;
            auto& frame_b_scale_delta = frame_b.scale - frame_base.scale;
            scale += (layer.blend * frame_base.scale) + (layer.anim_layer.blend * (frame_a_scale_delta * one_t + frame_b_scale_delta * info.t));
        }
        affine.pretranslate(trans);
        affine.rotate(quat.slerp(global_blend, ident).cast<float>());
        affine.scale(scale);
        if (i < puppet_layer.m_bone_overrides.size() && puppet_layer.m_bone_overrides[i].enabled) {
            affine = puppet_layer.m_bone_overrides[i].local_transform;
        }
        affine = parent * affine;
        m_bone_model_affines[i] = affine;
    }

    for (uint i = 0; i < m_final_affines.size(); i++) {
        m_final_affines[i] *= bones[i].offset_trans.matrix();
    }
    return m_final_affines;
}

const WPPuppet::Attachment* WPPuppet::FindAttachment(std::string_view name) const noexcept {
    auto it = std::find_if(attachments.begin(), attachments.end(), [name](const auto& attachment) {
        return attachment.name == name;
    });
    return it == attachments.end() ? nullptr : std::addressof(*it);
}

uint32_t WPPuppet::FindBoneIndex(std::string_view name) const noexcept {
    for (uint32_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) return i;
    }
    return 0xFFFFFFFFu;
}

const Affine3f& WPPuppet::BoneModelTransform(uint32_t index) const noexcept {
    static const Affine3f identity = Affine3f::Identity();
    if (index >= m_bone_model_affines.size()) return identity;
    return m_bone_model_affines[index];
}

static constexpr void genInterpolationInfo(WPPuppet::Animation::InterpolationInfo& info,
                                           double& cur, u32 length, double frame_time,
                                           double max_time) {
    cur          = std::fmod(cur, max_time);
    double _rate = cur / frame_time;

    info.frame_a = ((uint)_rate) % length;
    info.frame_b = (info.frame_a + 1) % length;
    info.t       = _rate - (double)info.frame_a;
}

double WPPuppet::Animation::EndTime() const noexcept {
    if (length <= 1 || frame_time <= std::numeric_limits<double>::epsilon()) return 0.0;
    return frame_time * static_cast<double>(length - 1);
}

WPPuppet::Animation::InterpolationInfo
WPPuppet::Animation::getInterpolationInfo(double* cur_time) const {
    InterpolationInfo _info;
    auto&             _cur_time = *cur_time;

    if (mode == PlayMode::Loop) {
        genInterpolationInfo(_info, _cur_time, (u32)length, frame_time, max_time);
    } else if (mode == PlayMode::Single) {
        // Clamp single-shot layers to the authored end frame so click-triggered animations
        // stay on their last pose instead of wrapping back to the start pose.
        _cur_time = std::clamp(_cur_time, 0.0, EndTime());
        const double frame_position = frame_time <= std::numeric_limits<double>::epsilon()
                                          ? 0.0
                                          : (_cur_time / frame_time);
        const auto last_frame = static_cast<idx>(std::max(length - 1, 0));
        _info.frame_a = std::min(static_cast<idx>(frame_position), last_frame);
        _info.frame_b = std::min(static_cast<idx>(_info.frame_a + 1), last_frame);
        _info.t = _info.frame_a == _info.frame_b
                      ? 0.0
                      : std::clamp(frame_position - static_cast<double>(_info.frame_a), 0.0, 1.0);
    } else if (mode == PlayMode::Mirror) {
        const auto _get_frame = [this](auto f) {
            return f >= length ? (length - 1) - (f - length) : f;
        };
        genInterpolationInfo(_info, _cur_time, (u32)length * 2, frame_time, max_time * 2.0f);
        _info.frame_a = _get_frame(_info.frame_a);
        _info.frame_b = _get_frame(_info.frame_b);
    }

    return _info;
}

void WPPuppetLayer::prepared(std::span<AnimationLayer> alayers) {
    m_layers.resize(alayers.size());
    m_bone_overrides.assign(m_puppet != nullptr ? m_puppet->bones.size() : 0, BoneOverride {});
    m_cached_skinning     = {};
    m_cached_frame_serial = std::numeric_limits<uint64_t>::max();

    if (m_puppet == nullptr) return;

    std::transform(
        alayers.rbegin(), alayers.rend(), m_layers.rbegin(), [this](const auto& layer) {
            const auto& anims = m_puppet->anims;

            auto it = std::find_if(anims.begin(), anims.end(), [&layer](auto& a) {
                return layer.id == a.id;
            });
            const bool has_animation = it != anims.end();

            auto runtime_layer = layer;
            if (has_animation && it->mode == WPPuppet::PlayMode::Single) {
                // Wallpaper Engine uses single-shot puppet layers for event-driven motions such
                // as clicks, so they must start paused until the script explicitly plays them.
                runtime_layer.playing = false;
            }

            // Keep the animation definition even when the authored/user initial state is hidden.
            // User properties may enable the layer later, and dropping the pointer here would make
            // false->true toggles impossible without reparsing the entire puppet.
            return Layer {
                .anim_layer = runtime_layer,
                .blend      = 0.0,
                .anim       = has_animation ? std::addressof(*it) : nullptr,
            };
        });
    RefreshBlendState();
}

void WPPuppetLayer::RefreshBlendState() noexcept {
    // Animation-layer visibility and blend are mutable user/script properties. Rebuilding the
    // normalized weights from the current runtime layer state keeps the base pose available when a
    // previously full-weight animation is disabled, which is what Wallpaper Engine expects.
    m_global_blend = 1.0;
    m_total_blend  = 0.0;

    for (const auto& layer : m_layers) {
        if (layer.anim != nullptr && layer.anim_layer.visible) {
            m_total_blend += layer.anim_layer.blend;
        }
    }

    double remaining_blend = 1.0;
    for (auto layer_it = m_layers.rbegin(); layer_it != m_layers.rend(); ++layer_it) {
        auto& layer = *layer_it;
        layer.blend = 0.0;
        if (layer.anim == nullptr || !layer.anim_layer.visible) continue;

        if (m_total_blend > 1.0) {
            layer.blend = layer.anim_layer.blend / m_total_blend;
            remaining_blend = 0.0;
        } else {
            layer.blend = remaining_blend * layer.anim_layer.blend;
            remaining_blend *= 1.0 - layer.anim_layer.blend;
            remaining_blend = remaining_blend < 0.0 ? 0.0 : remaining_blend;
        }
    }

    m_global_blend = remaining_blend;
    // The skinning matrices depend directly on the rebuilt blend weights, so cached bone matrices
    // must be invalidated even if the frame serial has not advanced yet.
    m_cached_skinning     = {};
    m_cached_frame_serial = std::numeric_limits<uint64_t>::max();
}

std::span<const Eigen::Affine3f> WPPuppetLayer::genFrame(double time) noexcept {
    m_cached_skinning = m_puppet->genFrame(*this, time);
    return m_cached_skinning;
}

std::span<const Eigen::Affine3f> WPPuppetLayer::AdvanceIfNeeded(double time,
                                                                uint64_t frame_serial) noexcept {
    if (!m_puppet) return {};
    if (m_cached_frame_serial != frame_serial) {
        m_cached_skinning     = m_puppet->genFrame(*this, time);
        m_cached_frame_serial = frame_serial;
    }
    return m_cached_skinning;
}

void WPPuppetLayer::updateInterpolation(double time) noexcept {
    for (auto& layer : m_layers) {
        if (layer) {
            double current_time = layer.anim_layer.cur_time;
            if (layer.anim_layer.playing) {
                current_time += time * layer.anim_layer.rate;
                if (layer.anim->mode == WPPuppet::PlayMode::Single) {
                    const double end_time = layer.anim->EndTime();
                    const bool reached_boundary =
                        current_time < 0.0 || current_time > end_time;
                    if (reached_boundary) {
                        // Stop single-shot layers at their terminal frame and arm a completion
                        // callback so scripts can replay them on the next explicit play().
                        current_time = layer.anim_layer.rate < 0.0 ? 0.0 : end_time;
                        layer.anim_layer.playing = false;
                        layer.anim_layer.pending_ended_callback = true;
                    }
                }
            }
            layer.interp_info = layer.anim->getInterpolationInfo(&current_time);
            layer.anim_layer.cur_time = current_time;
        }
    }
}

const WPPuppetLayer::AnimationLayer* WPPuppetLayer::AnimationLayerState(usize index) const noexcept {
    if (index >= m_layers.size()) return nullptr;
    return std::addressof(m_layers[index].anim_layer);
}

WPPuppetLayer::AnimationLayer* WPPuppetLayer::AnimationLayerState(usize index) noexcept {
    if (index >= m_layers.size()) return nullptr;
    return std::addressof(m_layers[index].anim_layer);
}

const WPPuppet::Animation* WPPuppetLayer::AnimationDefinition(usize index) const noexcept {
    if (index >= m_layers.size()) return nullptr;
    return m_layers[index].anim;
}

bool WPPuppetLayer::SetLocalBoneTransform(usize index, const Eigen::Affine3f& transform) noexcept {
    if (!m_puppet || index >= m_puppet->bones.size()) return false;
    if (index >= m_bone_overrides.size()) {
        m_bone_overrides.resize(m_puppet->bones.size());
    }

    m_bone_overrides[index].enabled = true;
    m_bone_overrides[index].local_transform = transform;
    m_cached_skinning = {};
    m_cached_frame_serial = std::numeric_limits<uint64_t>::max();
    return true;
}

WPPuppetLayer::WPPuppetLayer(std::shared_ptr<WPPuppet> pup): m_puppet(pup) {}
WPPuppetLayer::WPPuppetLayer()  = default;
WPPuppetLayer::~WPPuppetLayer() = default;
