#include "backend/scene/internal/transform/WPNodeTransformResolver.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace
{
bool Near(double lhs, double rhs, double epsilon = 0.0001) {
    return std::abs(lhs - rhs) <= epsilon;
}

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "node transform resolver test failure: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}
} // namespace

int main() {
    wallpaper::Scene scene;
    scene.ortho[0] = 200;
    scene.ortho[1] = 100;

    wallpaper::WPCameraParallax parallax;
    parallax.enable = true;
    parallax.amount = 2.0f;
    parallax.mouseinfluence = 0.0f;

    wallpaper::Map<void*, wallpaper::WPShaderValueData> node_data;
    wallpaper::Map<void*, Eigen::Matrix4d> model_cache;
    wallpaper::Map<void*, Eigen::Vector3f> parallax_cache;
    wallpaper::Map<void*, Eigen::Affine3f> attachment_cache;
    std::array<float, 2> mouse { 0.5f, 0.5f };
    wallpaper::SceneCamera camera(200, 100, 0.01f, 1000.0f);

    wallpaper::SceneNode parent;
    parent.SetTranslate({ 10.0f, 20.0f, 0.0f });
    parent.SetAlignmentOffset({ 5.0f, 0.0f, 0.0f });
    parent.UpdateTrans();

    wallpaper::SceneNode child;
    child.SetTranslate({ 2.0f, 3.0f, 0.0f });

    wallpaper::WPShaderValueData parent_data;
    parent_data.SetParallaxContract({ 0.1f, 0.2f });
    node_data[&parent] = parent_data;

    wallpaper::WPShaderValueData child_data;
    child_data.InheritParentTransform(&parent);
    node_data[&child] = child_data;

    wallpaper::WPNodeTransformResolver resolver(
        scene, parallax, node_data, model_cache, parallax_cache, attachment_cache, &camera, mouse, 0);

    const auto inherited = resolver.ResolveRawModelTransform(&child);
    Require(Near(inherited(0, 3), 12.0) && Near(inherited(1, 3), 23.0),
            "normal parent binding must inherit the authored transform");

    const auto parent_offset = resolver.ResolveParallaxOffset(&parent, &camera);
    const auto child_offset = resolver.ResolveParallaxOffset(&child, &camera);
    Require(Near(child_offset.x(), parent_offset.x()) &&
                Near(child_offset.y(), parent_offset.y()),
            "normal parent binding must inherit parent parallax");
    Require(Near(parent_offset.x(), 3.0) && Near(parent_offset.y(), 8.0),
            "parent parallax reference value mismatch");

    wallpaper::SceneNode independent_child;
    independent_child.SetTranslate({ 2.0f, 3.0f, 0.0f });
    wallpaper::WPShaderValueData independent_child_data;
    independent_child_data.SetParallaxContract({ 0.5f, 0.25f });
    independent_child_data.InheritParentTransform(&parent, false);
    node_data[&independent_child] = independent_child_data;

    model_cache.clear();
    parallax_cache.clear();
    wallpaper::WPNodeTransformResolver no_propagation_resolver(
        scene, parallax, node_data, model_cache, parallax_cache, attachment_cache, &camera, mouse, 0);

    const auto independent_model =
        no_propagation_resolver.ResolveRawModelTransform(&independent_child);
    Require(Near(independent_model(0, 3), 12.0) && Near(independent_model(1, 3), 23.0),
            "disablepropagation must not remove normal parent transform inheritance");

    const auto independent_offset =
        no_propagation_resolver.ResolveParallaxOffset(&independent_child, &camera);
    Require(Near(independent_offset.x(), 12.0) && Near(independent_offset.y(), 11.5),
            "disablepropagation child must use its own authored parallax depth");
    Require(! Near(independent_offset.x(), parent_offset.x()) ||
                ! Near(independent_offset.y(), parent_offset.y()),
            "disablepropagation child must not reuse the parent parallax offset");

    wallpaper::SceneNode aligned;
    aligned.SetAlignmentOffset({ 4.0f, 0.0f, 0.0f });
    Eigen::Affine3f local = Eigen::Affine3f::Identity();
    local.translate(Eigen::Vector3f { 14.0f, 0.0f, 0.0f });
    aligned.SetLocalAffine(local);
    const auto local_trans = aligned.GetLocalTrans();
    Require(Near(local_trans(0, 3), 14.0),
            "alignment offset must not mutate the authored local transform");

    return 0;
}
