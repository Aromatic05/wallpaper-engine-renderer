#pragma once

#include "WPShaderValueUpdater.hpp"

#include <optional>

namespace wallpaper
{

class Scene;
class SceneNode;
class SceneCamera;

class WPNodeTransformResolver {
public:
    WPNodeTransformResolver(Scene& scene, const WPCameraParallax& parallax,
                            Map<void*, WPShaderValueData>& node_data_map,
                            Map<void*, Eigen::Matrix4d>& model_transform_cache,
                            Map<void*, Eigen::Vector3f>& parallax_offset_cache,
                            Map<void*, Eigen::Affine3f>& attachment_transform_cache,
                            const SceneCamera* parallax_camera,
                            const std::array<float, 2>& mouse_pos,
                            uint64_t puppet_frame_serial);

    Eigen::Matrix4d ResolveParallaxedModelTransform(SceneNode* node,
                                                    const SceneCamera* camera,
                                                    bool apply_parallax);
    Eigen::Matrix4d ResolveRawModelTransform(SceneNode* node);
    Eigen::Vector3f ResolveParallaxOffset(SceneNode* node, const SceneCamera* camera);
    std::optional<Eigen::Affine3f> ResolveAttachmentLocalTransform(SceneNode* node);
    bool                           ApplyAttachment(SceneNode* node);
    void                           UpdateAttachmentParentIfNeeded(const WPShaderValueData& node_data);

private:
    const WPShaderValueData* FindNodeData(SceneNode* node) const;
    Eigen::Matrix4d          ResolveModelTransform(SceneNode* node, const WPShaderValueData* node_data);
    Eigen::Vector3f          ComputeParallaxOffset(SceneNode* node,
                                                   const WPShaderValueData& node_data,
                                                   const SceneCamera* camera);
    void ApplyResolvedParentDelta(SceneNode* target_parent,
                                  const WPShaderValueData& parent_data,
                                  Eigen::Affine3f& local_transform);
    void ApplyParentParallaxToAttachment(SceneNode* parent_node,
                                         const WPShaderValueData& parent_data,
                                         Eigen::Affine3f& local_transform);
    std::optional<Eigen::Affine3f> ResolveAttachmentLocalTransform(SceneNode* node,
                                                                   const WPShaderValueData& node_data);

    Scene&                         m_scene;
    const WPCameraParallax&        m_parallax;
    Map<void*, WPShaderValueData>& m_node_data_map;
    Map<void*, Eigen::Matrix4d>&   m_model_transform_cache;
    Map<void*, Eigen::Vector3f>&   m_parallax_offset_cache;
    Map<void*, Eigen::Affine3f>&   m_attachment_transform_cache;
    const SceneCamera*             m_parallax_camera;
    const std::array<float, 2>&    m_mouse_pos;
    uint64_t                       m_puppet_frame_serial;
};

} // namespace wallpaper
