#include "backend/scene/internal/parser/WPMdlParser.hpp"
#include "backend/scene/internal/parser/mdl/PuppetSemantics.hpp"

#include <Eigen/Geometry>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace
{
[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr, "puppet semantics test failure: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

bool Near(float lhs, float rhs, float epsilon = 0.0001f) {
    return std::abs(lhs - rhs) <= epsilon;
}

void RequireTranslation(const Eigen::Affine3f& transform,
                        const Eigen::Vector3f& expected,
                        std::string_view message) {
    if (! transform.translation().isApprox(expected, 0.0001f)) Fail(message);
}

wallpaper::WPMdl::Mesh BuildCentroidMesh() {
    wallpaper::WPMdl::Mesh mesh;
    mesh.positions = {
        { 10.0f, 0.0f, 0.0f },
        { 13.0f, 0.0f, 0.0f },
        { 10.0f, 3.0f, 0.0f },
        { 15.0f, 0.0f, 0.0f },
        { 18.0f, 0.0f, 0.0f },
        { 15.0f, 3.0f, 0.0f },
    };
    mesh.blend_indices = {
        { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
        { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 },
    };
    mesh.blend_weights = {
        { 1.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f },
    };
    mesh.indices = { { 0, 1, 2 }, { 3, 4, 5 } };
    return mesh;
}

std::shared_ptr<wallpaper::WPPuppet> BuildTwoBonePuppet(bool worldAnchored) {
    auto puppet = std::make_shared<wallpaper::WPPuppet>();
    puppet->bones.resize(2);

    auto& root = puppet->bones[0];
    root.name = "root";
    root.file_parent = wallpaper::WPPuppet::NO_PARENT;
    root.local_bind = Eigen::Affine3f::Identity();
    root.local_bind.pretranslate(Eigen::Vector3f(10.0f, 0.0f, 0.0f));

    auto& child = puppet->bones[1];
    child.name = "child";
    child.file_parent = 0;
    child.local_bind = Eigen::Affine3f::Identity();
    child.local_bind.pretranslate(
        worldAnchored ? Eigen::Vector3f(15.0f, 0.0f, 0.0f)
                      : Eigen::Vector3f(5.0f, 0.0f, 0.0f));
    return puppet;
}

void TestNormalHierarchyBindPose() {
    wallpaper::WPMdl mdl;
    mdl.header.mdlv = 17;
    mdl.mdls = 3;
    mdl.puppet = BuildTwoBonePuppet(false);
    mdl.meshes.push_back(BuildCentroidMesh());

    wallpaper::ApplyWPMdlPuppetSemantics(mdl);
    Require(! mdl.puppet->world_anchored_bones,
            "MDLV17 must keep chained bind semantics");
    Require(mdl.puppet->bones[1].bind_parent == 0
                && mdl.puppet->bones[1].anim_parent == 0,
            "normal puppet parent roles must match the file parent");

    mdl.puppet->bones[0].file_world_bind = Eigen::Affine3f::Identity();
    mdl.puppet->bones[0].file_world_bind.pretranslate(Eigen::Vector3f(99.0f, 0.0f, 0.0f));
    mdl.puppet->bones[0].has_file_world_bind = true;
    mdl.puppet->prepared();

    RequireTranslation(mdl.puppet->bones[0].world_bind,
                       Eigen::Vector3f(10.0f, 0.0f, 0.0f),
                       "MDLE must not replace the validated MDLS root bind");
    RequireTranslation(mdl.puppet->bones[1].world_bind,
                       Eigen::Vector3f(15.0f, 0.0f, 0.0f),
                       "normal child world bind must follow the bind parent");

    wallpaper::WPPuppetLayer layer(mdl.puppet);
    std::array<wallpaper::WPPuppetLayer::AnimationLayer, 0> noLayers {};
    layer.prepared(std::span<wallpaper::WPPuppetLayer::AnimationLayer>(noLayers));
    const auto skinning = layer.genFrame(0.0);
    Require(skinning.size() == 2, "normal puppet skinning count mismatch");
    Require(skinning[0].matrix().isApprox(Eigen::Matrix4f::Identity(), 0.0001f)
                && skinning[1].matrix().isApprox(Eigen::Matrix4f::Identity(), 0.0001f),
            "normal bind pose must produce identity skinning matrices");
}

void TestMDLV21CentroidAndAnimationInheritance() {
    wallpaper::WPMdl mdl;
    mdl.header.mdlv = 21;
    mdl.mdls = 3;
    mdl.puppet = BuildTwoBonePuppet(true);
    mdl.meshes.push_back(BuildCentroidMesh());

    wallpaper::ApplyWPMdlPuppetSemantics(mdl);
    Require(mdl.puppet->world_anchored_bones,
            "MDLV21 must enable world-anchored bind semantics");
    Require(mdl.puppet->bones[0].bind_parent == wallpaper::WPPuppet::NO_PARENT
                && mdl.puppet->bones[1].bind_parent == wallpaper::WPPuppet::NO_PARENT,
            "MDLV21 bind hierarchy must be flat");
    Require(mdl.puppet->bones[1].anim_parent == 0
                && mdl.puppet->bones[1].file_parent == 0,
            "MDLV21 must preserve the file parent for animation");
    Require(mdl.puppet->bones[0].vertex_centroid_offset.isApprox(
                Eigen::Vector3f(1.0f, 1.0f, 0.0f), 0.0001f)
                && mdl.puppet->bones[1].vertex_centroid_offset.isApprox(
                    Eigen::Vector3f(1.0f, 1.0f, 0.0f), 0.0001f),
            "MDLV21 area-weighted centroid offsets mismatch");

    wallpaper::WPPuppet::Animation animation;
    animation.id = 1;
    animation.name = "root-motion";
    animation.fps = 1.0;
    animation.length = 2;
    animation.mode = wallpaper::WPPuppet::PlayMode::Loop;
    animation.bframes_array.resize(2);
    animation.bframes_array[0].frames.resize(2);
    animation.bframes_array[0].frames[0].position = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
    animation.bframes_array[0].frames[1].position = Eigen::Vector3f(14.0f, 0.0f, 0.0f);
    for (auto& frame : animation.bframes_array[0].frames) {
        frame.angle = Eigen::Vector3f::Zero();
        frame.scale = Eigen::Vector3f::Ones();
        frame.quaternion = Eigen::Quaterniond::Identity();
    }
    mdl.puppet->anims.push_back(animation);
    mdl.puppet->prepared();

    RequireTranslation(mdl.puppet->bones[0].world_bind,
                       Eigen::Vector3f(11.0f, 1.0f, 0.0f),
                       "MDLV21 root bind must include the centroid offset");
    RequireTranslation(mdl.puppet->bones[1].world_bind,
                       Eigen::Vector3f(16.0f, 1.0f, 0.0f),
                       "MDLV21 child bind must stay world anchored");

    wallpaper::WPPuppetLayer layer(mdl.puppet);
    std::array<wallpaper::WPPuppetLayer::AnimationLayer, 1> layers {
        wallpaper::WPPuppetLayer::AnimationLayer { .id = 1, .rate = 1.0, .blend = 1.0 },
    };
    layer.prepared(layers);

    const auto bindSkinning = layer.genFrame(0.0);
    Require(bindSkinning[0].matrix().isApprox(Eigen::Matrix4f::Identity(), 0.0001f)
                && bindSkinning[1].matrix().isApprox(Eigen::Matrix4f::Identity(), 0.0001f),
            "MDLV21 bind pose must produce identity skinning matrices");

    const auto animatedSkinning = layer.genFrame(1.0);
    RequireTranslation(animatedSkinning[0],
                       Eigen::Vector3f(4.0f, 0.0f, 0.0f),
                       "MDLV21 root animation delta mismatch");
    RequireTranslation(animatedSkinning[1],
                       Eigen::Vector3f(4.0f, 0.0f, 0.0f),
                       "MDLV21 child must inherit its parent's animated delta");
    RequireTranslation(mdl.puppet->BoneModelTransform(1),
                       Eigen::Vector3f(20.0f, 1.0f, 0.0f),
                       "MDLV21 child model transform mismatch after inherited animation");
    Require(Near(mdl.puppet->bones[1].file_world_bind.translation().x(), 0.0f),
            "unset MDLE payload must remain observational only");
}
} // namespace

int main() {
    TestNormalHierarchyBindPose();
    TestMDLV21CentroidAndAnimationInheritance();
    return 0;
}
