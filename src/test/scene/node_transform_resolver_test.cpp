#include "backend/scene/internal/transform/WPNodeTransformResolver.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"

#include <cassert>
#include <cmath>

namespace
{

bool Near(double lhs, double rhs, double epsilon = 0.0001) {
    return std::abs(lhs - rhs) <= epsilon;
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
    assert(Near(inherited(0, 3), 12.0));
    assert(Near(inherited(1, 3), 23.0));

    const auto parent_offset = resolver.ResolveParallaxOffset(&parent, &camera);
    const auto child_offset = resolver.ResolveParallaxOffset(&child, &camera);
    assert(Near(child_offset.x(), parent_offset.x()));
    assert(Near(child_offset.y(), parent_offset.y()));
    assert(Near(parent_offset.x(), 3.0));
    assert(Near(parent_offset.y(), 8.0));

    wallpaper::SceneNode aligned;
    aligned.SetAlignmentOffset({ 4.0f, 0.0f, 0.0f });
    Eigen::Affine3f local = Eigen::Affine3f::Identity();
    local.translate(Eigen::Vector3f { 14.0f, 0.0f, 0.0f });
    aligned.SetLocalAffine(local);
    const auto local_trans = aligned.GetLocalTrans();
    assert(Near(local_trans(0, 3), 14.0));

    return 0;
}
