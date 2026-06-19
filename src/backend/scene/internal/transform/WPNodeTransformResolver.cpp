#include "WPNodeTransformResolver.hpp"

#include "scene/Scene.h"
#include "scene/SceneNode.h"
#include "WPImageAlignment.hpp"

#include <Eigen/Geometry>

using namespace wallpaper;
using namespace Eigen;

WPNodeTransformResolver::WPNodeTransformResolver(
    Scene& scene, const WPCameraParallax& parallax,
    Map<void*, WPShaderValueData>& node_data_map,
    Map<void*, Matrix4d>& model_transform_cache,
    Map<void*, Vector3f>& parallax_offset_cache,
    Map<void*, Affine3f>& attachment_transform_cache,
    const SceneCamera* parallax_camera,
    const std::array<float, 2>& mouse_pos, uint64_t puppet_frame_serial)
    : m_scene(scene),
      m_parallax(parallax),
      m_node_data_map(node_data_map),
      m_model_transform_cache(model_transform_cache),
      m_parallax_offset_cache(parallax_offset_cache),
      m_attachment_transform_cache(attachment_transform_cache),
      m_parallax_camera(parallax_camera),
      m_mouse_pos(mouse_pos),
      m_puppet_frame_serial(puppet_frame_serial) {}

Matrix4d WPNodeTransformResolver::ResolveParallaxedModelTransform(SceneNode* node,
                                                                  const SceneCamera* camera,
                                                                  bool apply_parallax) {
    const auto* node_data = FindNodeData(node);
    Matrix4d    model_trans = ResolveModelTransform(node, node_data);
    if (node_data != nullptr && apply_parallax && node_data->AppliesModelParallax()) {
        const auto parallax_offset = ComputeParallaxOffset(node, *node_data, camera);
        model_trans =
            Affine3d(Eigen::Translation3d(parallax_offset.cast<double>())).matrix() * model_trans;
    }
    return model_trans;
}

Matrix4d WPNodeTransformResolver::ResolveRawModelTransform(SceneNode* node) {
    const auto* node_data = FindNodeData(node);
    return ResolveModelTransform(node, node_data);
}

Vector3f WPNodeTransformResolver::ResolveParallaxOffset(SceneNode* node, const SceneCamera* camera) {
    const auto* node_data = FindNodeData(node);
    if (node_data == nullptr) return Vector3f::Zero();
    return ComputeParallaxOffset(node, *node_data, camera);
}

std::optional<Affine3f> WPNodeTransformResolver::ResolveAttachmentLocalTransform(SceneNode* node) {
    const auto* node_data = FindNodeData(node);
    if (node_data == nullptr) return std::nullopt;
    return ResolveAttachmentLocalTransform(node, *node_data);
}

bool WPNodeTransformResolver::ApplyAttachment(SceneNode* node) {
    auto local_transform = ResolveAttachmentLocalTransform(node);
    if (! local_transform.has_value()) return false;
    node->SetLocalAffine(*local_transform);
    return true;
}

void WPNodeTransformResolver::UpdateAttachmentParentIfNeeded(const WPShaderValueData& node_data) {
    if (node_data.TransformParent() == nullptr ||
        ! exists(m_node_data_map, node_data.TransformParent())) {
        return;
    }

    auto& parent_data = m_node_data_map.at(node_data.TransformParent());
    if (! parent_data.IsBoneAttached()) return;

    ApplyAttachment(node_data.TransformParent());
}

const WPShaderValueData* WPNodeTransformResolver::FindNodeData(SceneNode* node) const {
    if (node == nullptr || ! exists(m_node_data_map, node)) return nullptr;
    return std::addressof(m_node_data_map.at(node));
}

Matrix4d WPNodeTransformResolver::ResolveModelTransform(SceneNode* node,
                                                        const WPShaderValueData* node_data) {
    if (node == nullptr) return Matrix4d::Identity();
    if (exists(m_model_transform_cache, node)) return m_model_transform_cache.at(node);

    Matrix4d resolved = Matrix4d::Identity();
    if (node_data != nullptr && node_data->InheritsSceneParentTransform() &&
        node_data->TransformParent() != nullptr &&
        exists(m_node_data_map, node_data->TransformParent())) {
        const auto& parent_data = m_node_data_map.at(node_data->TransformParent());
        auto*       parent_node  = node_data->TransformParent();
        const auto  parent_model =
            RemoveImageAlignmentOffsetFromModel(ResolveModelTransform(parent_node, &parent_data),
                                                parent_node->AlignmentOffset());
        resolved = parent_model * node->GetLocalTrans();
    } else {
        node->UpdateTrans();
        resolved = node->ModelTrans();
    }

    m_model_transform_cache[node] = resolved;
    return resolved;
}

Vector3f WPNodeTransformResolver::ComputeParallaxOffset(SceneNode* node,
                                                        const WPShaderValueData& node_data,
                                                        const SceneCamera* camera) {
    if (node == nullptr || camera == nullptr || ! m_parallax.enable) return Vector3f::Zero();
    if (exists(m_parallax_offset_cache, node)) return m_parallax_offset_cache.at(node);

    Vector3f offset = Vector3f::Zero();
    if (node_data.parallax_anchor != nullptr &&
        exists(m_node_data_map, node_data.parallax_anchor)) {
        const auto& parent_data = m_node_data_map.at(node_data.parallax_anchor);
        if (! parent_data.IsBoneAttached()) {
            offset = ComputeParallaxOffset(node_data.parallax_anchor, parent_data, camera);
        }
    } else {
        const auto model_trans = ResolveModelTransform(node, &node_data);
        Vector3f   node_pos((float)model_trans(0, 3),
                          (float)model_trans(1, 3),
                          (float)model_trans(2, 3));
        Vector2f depth(node_data.parallaxDepth[0], node_data.parallaxDepth[1]);

        Vector2f ortho { (float)m_scene.ortho[0], (float)m_scene.ortho[1] };
        Vector2f mouse_vec =
            Scaling(1.0f, -1.0f) * (Vector2f { 0.5f, 0.5f } - Vector2f(&m_mouse_pos[0]));
        mouse_vec = mouse_vec.cwiseProduct(ortho) * m_parallax.mouseinfluence;

        Vector3f cam_pos = camera->GetPosition().cast<float>();
        Vector2f para_vec =
            (node_pos.head<2>() - cam_pos.head<2>() + mouse_vec).cwiseProduct(depth) *
            m_parallax.amount;
        offset = Vector3f(para_vec.x(), para_vec.y(), 0.0f);
    }

    m_parallax_offset_cache[node] = offset;
    return offset;
}

void WPNodeTransformResolver::ApplyResolvedParentDelta(SceneNode* target_parent,
                                                       const WPShaderValueData& parent_data,
                                                       Affine3f& local_transform) {
    if (target_parent == nullptr) return;

    target_parent->UpdateTrans();
    const auto parent_actual_model   = target_parent->ModelTrans();
    const auto parent_resolved_model = ResolveModelTransform(target_parent, &parent_data);
    const auto parent_actual_linear  = parent_actual_model.block<3, 3>(0, 0);
    if (std::abs(parent_actual_linear.determinant()) <= 1e-12) return;

    const auto resolved_delta = parent_actual_model.inverse() * parent_resolved_model;
    local_transform           = Affine3f(resolved_delta.cast<float>()) * local_transform;
}

void WPNodeTransformResolver::ApplyParentParallaxToAttachment(SceneNode* parent_node,
                                                              const WPShaderValueData& parent_data,
                                                              Affine3f& local_transform) {
    if (parent_node == nullptr || m_parallax_camera == nullptr || ! m_parallax.enable) return;
    if (parent_data.IsBoneAttached()) {
        // Nested attachments resolve their parent SceneNode local transform before the child is
        // evaluated. That resolved parent local transform already carries the inherited camera
        // parallax needed to keep the attached artwork locked to the parent puppet. Injecting the
        // same parent parallax again here makes the offset accumulate once per attachment depth
        // (body -> leg -> shoe), which pulls shoe overlays away from the base foot artwork. Direct
        // attachments to normal puppet layers still fall through because their parent parallax is
        // shader-only and must be converted into the attachment's local space.
        return;
    }

    const auto parent_parallax = ComputeParallaxOffset(parent_node, parent_data, m_parallax_camera);
    const auto parent_model    = ResolveModelTransform(parent_node, &parent_data);
    Matrix3f   parent_linear   = parent_model.block<3, 3>(0, 0).cast<float>();
    Vector3f   parent_parallax_local = parent_parallax;
    if (std::abs(parent_linear.determinant()) > 1e-6f) {
        parent_parallax_local = parent_linear.inverse() * parent_parallax;
    }
    local_transform.translation() += parent_parallax_local;
}

std::optional<Affine3f> WPNodeTransformResolver::ResolveAttachmentLocalTransform(
    SceneNode* node, const WPShaderValueData& node_data) {
    if (node == nullptr || ! node_data.IsBoneAttached() || node_data.TransformParent() == nullptr ||
        ! exists(m_node_data_map, node_data.TransformParent())) {
        return std::nullopt;
    }

    if (exists(m_attachment_transform_cache, node)) {
        return m_attachment_transform_cache.at(node);
    }

    auto* parent_node = node_data.TransformParent();
    auto& parent_data = m_node_data_map.at(parent_node);
    if (parent_data.IsBoneAttached()) {
        auto parent_transform = ResolveAttachmentLocalTransform(parent_node, parent_data);
        if (! parent_transform.has_value()) return std::nullopt;
        parent_node->SetLocalAffine(*parent_transform);
    }

    if (! parent_data.puppet_layer.hasPuppet()) return std::nullopt;

    parent_data.puppet_layer.AdvanceIfNeeded(m_scene.frameTime, m_puppet_frame_serial);
    const auto* parent_puppet = parent_data.puppet_layer.Puppet();
    if (parent_puppet == nullptr) return std::nullopt;

    Affine3f local_transform =
        parent_puppet->BoneModelTransform(node_data.transform_binding.bone_index) *
        node_data.transform_binding.anchor_transform * node_data.transform_binding.local_transform;
    ApplyResolvedParentDelta(parent_node, parent_data, local_transform);
    ApplyParentParallaxToAttachment(parent_node, parent_data, local_transform);
    m_attachment_transform_cache[node] = local_transform;
    return local_transform;
}
