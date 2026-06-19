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
    enum class PlayMode
    {
        Loop,
        Mirror,
        Single
    };
    struct Bone {
        std::string     name;
        Eigen::Affine3f transform { Eigen::Affine3f::Identity() };
        uint32_t        parent { 0xFFFFFFFFu };

        bool noParent() const { return parent == 0xFFFFFFFFu; }
        // prepared
        Eigen::Affine3f offset_trans { Eigen::Affine3f::Identity() };
        /*
        Eigen::Vector3f world_axis_x;
        Eigen::Vector3f world_axis_y;
        Eigen::Vector3f world_axis_z;
        */
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
