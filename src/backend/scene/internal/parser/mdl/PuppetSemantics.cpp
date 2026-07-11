#include "PuppetSemantics.hpp"

#include "../WPMdlParser.hpp"

#include <Eigen/Geometry>

#include <vector>

namespace wallpaper
{
void ApplyWPMdlPuppetSemantics(WPMdl& mdl) {
    if (! mdl.puppet) return;

    auto& puppet = *mdl.puppet;
    puppet.world_anchored_bones = mdl.header.mdlv == 21;
    for (auto& bone : puppet.bones) {
        bone.bind_parent = puppet.world_anchored_bones ? WPPuppet::NO_PARENT
                                                       : bone.file_parent;
        bone.anim_parent = bone.file_parent;
        bone.vertex_centroid_offset.setZero();
    }

    if (mdl.mdls < 3 || mdl.meshes.empty() || puppet.bones.empty()) return;

    const auto boneCount = puppet.bones.size();
    std::vector<Eigen::Vector3d> weightedPositions(boneCount, Eigen::Vector3d::Zero());
    std::vector<double> weights(boneCount, 0.0);

    const auto contribute = [&](const WPMdl::Mesh& mesh) {
        if (mesh.blend_indices.empty()) return;
        const bool hasWeights = ! mesh.blend_weights.empty();
        const auto weightAt = [&](usize vertex, usize slot) {
            return hasWeights ? mesh.blend_weights[vertex][slot]
                              : (slot == 0 ? 1.0f : 0.0f);
        };
        const usize slots = hasWeights ? 4 : 1;

        if (! mesh.indices.empty()) {
            for (const auto& triangle : mesh.indices) {
                if (triangle[0] >= mesh.positions.size()
                    || triangle[1] >= mesh.positions.size()
                    || triangle[2] >= mesh.positions.size()) {
                    continue;
                }
                const Eigen::Vector3d p0(mesh.positions[triangle[0]][0],
                                         mesh.positions[triangle[0]][1],
                                         mesh.positions[triangle[0]][2]);
                const Eigen::Vector3d p1(mesh.positions[triangle[1]][0],
                                         mesh.positions[triangle[1]][1],
                                         mesh.positions[triangle[1]][2]);
                const Eigen::Vector3d p2(mesh.positions[triangle[2]][0],
                                         mesh.positions[triangle[2]][1],
                                         mesh.positions[triangle[2]][2]);
                const auto triangleCentroid = (p0 + p1 + p2) / 3.0;
                const double area = 0.5 * (p1 - p0).cross(p2 - p0).norm();
                if (area <= 0.0) continue;

                for (usize corner = 0; corner < 3; ++corner) {
                    const auto vertex = static_cast<usize>(triangle[corner]);
                    for (usize slot = 0; slot < slots; ++slot) {
                        const float weight = weightAt(vertex, slot);
                        const auto boneIndex = mesh.blend_indices[vertex][slot];
                        if (weight <= 0.0f || boneIndex >= boneCount) continue;
                        const double contribution = area * static_cast<double>(weight) / 3.0;
                        weightedPositions[boneIndex] += triangleCentroid * contribution;
                        weights[boneIndex] += contribution;
                    }
                }
            }
        } else {
            for (usize vertex = 0; vertex < mesh.positions.size(); ++vertex) {
                const Eigen::Vector3d position(mesh.positions[vertex][0],
                                               mesh.positions[vertex][1],
                                               mesh.positions[vertex][2]);
                for (usize slot = 0; slot < slots; ++slot) {
                    const float weight = weightAt(vertex, slot);
                    const auto boneIndex = mesh.blend_indices[vertex][slot];
                    if (weight <= 0.0f || boneIndex >= boneCount) continue;
                    weightedPositions[boneIndex] += position * static_cast<double>(weight);
                    weights[boneIndex] += static_cast<double>(weight);
                }
            }
        }
    };

    for (const auto& mesh : mdl.meshes) contribute(mesh);
    for (usize index = 0; index < boneCount; ++index) {
        if (weights[index] <= 0.0) continue;
        const auto centroid = (weightedPositions[index] / weights[index]).cast<float>();
        puppet.bones[index].vertex_centroid_offset =
            centroid - puppet.bones[index].local_bind.translation();
    }
}
} // namespace wallpaper
