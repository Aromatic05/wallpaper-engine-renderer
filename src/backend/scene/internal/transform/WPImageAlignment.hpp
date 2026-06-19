#pragma once

#include <array>
#include <string_view>

#include <Eigen/Dense>
#include <Eigen/Geometry>

namespace wallpaper
{

inline Eigen::Vector3f ResolveImageAlignmentOffset(std::string_view alignment,
                                                   const Eigen::Vector2f& size) {
    struct AlignmentAxisContribution {
        std::string_view token;
        int              axis;
        float            direction;
    };

    // Wallpaper Engine image layers author `origin` as the pivot implied by `alignment`. The
    // renderer keeps that origin untouched and moves the image quad in local space instead, so
    // rotation and scale scripts orbit around the authored pivot rather than a pre-shifted
    // translation that only looked correct for static layers.
    constexpr std::array kContributions {
        AlignmentAxisContribution { "left", 0, 1.0f },
        AlignmentAxisContribution { "right", 0, -1.0f },
        AlignmentAxisContribution { "top", 1, -1.0f },
        AlignmentAxisContribution { "bottom", 1, 1.0f },
    };

    Eigen::Vector3f offset = Eigen::Vector3f::Zero();
    const Eigen::Vector2f half_size = size * 0.5f;
    for (const auto& contribution : kContributions) {
        if (alignment.find(contribution.token) == std::string_view::npos) continue;
        offset[contribution.axis] += half_size[contribution.axis] * contribution.direction;
    }
    return offset;
}

inline Eigen::Vector3f ResolveImageAlignmentOffset(std::string_view alignment,
                                                   const std::array<float, 2>& size) {
    return ResolveImageAlignmentOffset(alignment, Eigen::Vector2f { size[0], size[1] });
}

inline Eigen::Matrix4d RemoveImageAlignmentOffsetFromModel(
    const Eigen::Matrix4d& model,
    const Eigen::Vector3f& alignment_offset) {
    if (alignment_offset.isZero(0.0f)) return model;

    // SceneNode::GetLocalTrans() appends alignment as the final local-space translate so the
    // layer's own mesh is visually placed around the authored pivot. Children and detached final
    // writers must inherit that authored pivot, not the mesh placement offset, so callers remove
    // the appended translate by post-multiplying its inverse.
    Eigen::Affine3d inverse_alignment = Eigen::Affine3d::Identity();
    inverse_alignment.translate((-alignment_offset).cast<double>());
    return model * inverse_alignment.matrix();
}

} // namespace wallpaper
