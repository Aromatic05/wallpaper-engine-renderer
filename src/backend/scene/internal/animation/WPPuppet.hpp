#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <span>
#include <limits>
#include <Eigen/Geometry>

#include "core/Literals.hpp"

namespace wallpaper
{

class WPPuppetLayer;

class WPPuppet {
public:
    static constexpr uint32_t NO_PARENT = 0xFFFFFFFFu;

    enum class PlayMode
    {
        Loop,
        Mirror,
        Single
    };
    struct Bone {
        std::string     name;
        Eigen::Affine3f local_bind { Eigen::Affine3f::Identity() };
        uint32_t        file_parent { NO_PARENT };
        uint32_t        bind_parent { NO_PARENT };
        uint32_t        anim_parent { NO_PARENT };
        Eigen::Affine3f world_bind { Eigen::Affine3f::Identity() };
        Eigen::Affine3f inv_bind { Eigen::Affine3f::Identity() };
        Eigen::Vector3f vertex_centroid_offset { Eigen::Vector3f::Zero() };
        Eigen::Vector3f file_skin_pivot { Eigen::Vector3f::Zero() };
        Eigen::Matrix4f file_skin_matrix { Eigen::Matrix4f::Identity() };
        bool            has_file_skin_pivot { false };
        std::string     simulation_json;
        Eigen::Affine3f file_world_bind { Eigen::Affine3f::Identity() };
        bool            has_file_world_bind { false };

        bool noFileParent() const { return file_parent == NO_PARENT; }
        bool noBindParent() const { return bind_parent == NO_PARENT; }
        bool noAnimParent() const { return anim_parent == NO_PARENT; }
        bool noParent() const { return noAnimParent(); }
    };
    struct Attachment {
        std::string     name;
        uint32_t        bone_index { 0xFFFFFFFFu };
        Eigen::Affine3f transform { Eigen::Affine3f::Identity() };
    };
    struct BoneFrame {
        Eigen::Vector3f position;
        Eigen::Vector3f angle;
        Eigen::Vector3f scale;

        // prepared
        Eigen::Quaterniond quaternion;
    };
    struct Animation {
        i32         id;
        double      fps;
        i32         length;
        PlayMode    mode;
        std::string name;

        struct BoneFrames {
            std::vector<BoneFrame> frames;
        };
        std::vector<BoneFrames> bframes_array;

        // prepared
        double max_time;
        double frame_time;
        struct InterpolationInfo {
            idx    frame_a;
            idx    frame_b;
            double t;
        };
        // Single-shot puppet animations must stop on their authored last frame instead of
        // wrapping back to frame zero like a looped idle layer.
        double            EndTime() const noexcept;
        InterpolationInfo getInterpolationInfo(double* cur_time) const;
    };

public:
    std::vector<Bone>      bones;
    std::vector<Attachment> attachments;
    std::vector<Animation> anims;
    bool                      world_anchored_bones { false };

    std::span<const Eigen::Affine3f> genFrame(WPPuppetLayer&, double time) noexcept;
    void                             prepared();
    const Attachment*                FindAttachment(std::string_view name) const noexcept;
    uint32_t                         FindBoneIndex(std::string_view name) const noexcept;
    const Eigen::Affine3f&           BoneModelTransform(uint32_t index) const noexcept;

private:
    std::vector<Eigen::Affine3f> m_final_affines;
    std::vector<Eigen::Affine3f> m_bone_model_affines;
};

class WPPuppetLayer {
    friend class WPPuppet;

public:
    WPPuppetLayer();
    WPPuppetLayer(std::shared_ptr<WPPuppet>);
    ~WPPuppetLayer();

    bool hasPuppet() const { return (bool)m_puppet; };

    struct AnimationLayer {
        i32    id { 0 };
        double rate { 1.0f };
        double blend { 1.0f };
        bool   visible { true };
        // Schema metadata preserved for newer animation-layer revisions. Runtime blending still
        // consumes id/rate/blend/visible until Stage 3 implements the authored transition policy.
        i32         layer_id { 0 };
        std::string name;
        bool        additive { false };
        bool        blendin { false };
        bool        blendout { false };
        double      blendtime { 0.0 };
        double cur_time { 0.0f };
        bool   playing { true };
        // NotifyAnimationLayersAdvanced consumes this latch after it fires ended callbacks.
        // This lets single-shot layers report completion without pretending that they wrapped.
        bool   pending_ended_callback { false };
    };

    void prepared(std::span<AnimationLayer>);
    // Runtime user properties can toggle an animation layer after the puppet has been prepared.
    // The layer list and animation pointers stay stable, but the normalized blend weights and the
    // fallback base-pose weight must be rebuilt whenever visibility or blend changes, otherwise a
    // disabled full-weight layer can leave the base pose with zero influence and collapse the mesh.
    void RefreshBlendState() noexcept;

    std::span<const Eigen::Affine3f> genFrame(double time) noexcept;
    std::span<const Eigen::Affine3f> AdvanceIfNeeded(double time, uint64_t frame_serial) noexcept;
    std::span<const Eigen::Affine3f> SkinningMatrices() const noexcept { return m_cached_skinning; }
    const WPPuppet*                  Puppet() const noexcept { return m_puppet.get(); }
    usize                            AnimationLayerCount() const noexcept { return m_layers.size(); }
    const AnimationLayer*            AnimationLayerState(usize index) const noexcept;
    AnimationLayer*                  AnimationLayerState(usize index) noexcept;
    const WPPuppet::Animation*       AnimationDefinition(usize index) const noexcept;
    bool SetLocalBoneTransform(usize index, const Eigen::Affine3f& transform) noexcept;

    void updateInterpolation(double time) noexcept;

private:
    struct Layer {
        AnimationLayer                         anim_layer;
        double                                 blend;
        const WPPuppet::Animation*             anim { nullptr };
        WPPuppet::Animation::InterpolationInfo interp_info {};

        operator bool() const noexcept { return anim != nullptr; };
    };
    struct BoneOverride {
        bool            enabled { false };
        Eigen::Affine3f local_transform { Eigen::Affine3f::Identity() };
    };

    double m_global_blend { 1.0 };
    double m_total_blend { 0.0 };

    std::vector<Layer>              m_layers;
    std::vector<BoneOverride>       m_bone_overrides;
    std::shared_ptr<WPPuppet>       m_puppet;
    std::span<const Eigen::Affine3f> m_cached_skinning {};
    uint64_t                        m_cached_frame_serial { std::numeric_limits<uint64_t>::max() };
};

} // namespace wallpaper
