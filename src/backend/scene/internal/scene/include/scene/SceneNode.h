#pragma once
#include <list>
#include <vector>
#include <memory>
#include <string>
#include <utility>
#include <Eigen/Dense>
#include "SceneMesh.h"
#include "SceneCamera.h"

#include "core/Literals.hpp"
#include "core/NoCopyMove.hpp"

namespace wallpaper
{

class SceneTextPrimitive;

class SceneNode : NoCopy, NoMove {
public:
    SceneNode()
        : m_name(),
          m_dirty(true),
          m_translate(Eigen::Vector3f::Zero()),
          m_scale { 1.0f, 1.0f, 1.0f },
          m_rotation(Eigen::Vector3f::Zero()) {}
    SceneNode(const Eigen::Vector3f& translate, const Eigen::Vector3f& scale,
              const Eigen::Vector3f& rotation, const std::string& name = "")
        : m_name(name),
          m_dirty(true),
          m_translate(translate),
          m_scale(scale),
          m_rotation(rotation) {};

    const auto& Camera() const { return m_cameraName; }
    void        SetCamera(const std::string& name) { m_cameraName = name; }
    const auto& Name() const { return m_name; }
    void        SetName(std::string name) { m_name = std::move(name); }
    void        AddMesh(std::shared_ptr<SceneMesh> mesh) { m_mesh = mesh; }
    // Text is now a first-class scene renderable. The node keeps an optional text primitive next
    // to the mesh slot so the render graph can emit a dedicated text pass directly.
    void        AddText(std::shared_ptr<SceneTextPrimitive> text) { m_text = std::move(text); }
    void        AppendChild(std::shared_ptr<SceneNode> sub) {
               sub->m_parent = this;
               // Reparenting changes the model-space basis for the entire child branch even when
               // the child already cached a clean local-only matrix during scene construction.
               // Force the full branch dirty here instead of using MarkTransDirty(), because that
               // helper intentionally stops when the current node is already dirty.
               sub->MarkTransSubtreeDirty();
               m_children.push_back(sub);
    }
    bool        RemoveChild(SceneNode* child) {
               for (auto it = m_children.begin(); it != m_children.end(); ++it) {
                   if (it->get() == child) {
                       auto removed = *it;
                       removed->m_parent = nullptr;
                       // Removing a parent also changes every cached model matrix below the
                       // removed node: descendants must drop the old inherited transform before
                       // the shared_ptr is released from this child list.
                       removed->MarkTransSubtreeDirty();
                       m_children.erase(it);
                       return true;
                   }
               }
               return false;
    }
    Eigen::Matrix4d GetLocalTrans() const;

    const auto& Translate() const { return m_translate; }
    const auto& Rotation() const { return m_rotation; }
    const auto& Scale() const { return m_scale; }
    const auto& AlignmentOffset() const { return m_alignmentOffset; }
    void        SetRotation(Eigen::Vector3f v) {
        m_rotation = v;
        MarkTransDirty();
    }
    void        SetScale(Eigen::Vector3f v) {
        m_scale = v;
        MarkTransDirty();
    }
    void        SetTranslate(Eigen::Vector3f v) {
        m_translate = v;
        MarkTransDirty();
    }
    void        SetAlignmentOffset(Eigen::Vector3f v) {
        m_alignmentOffset = v;
        MarkTransDirty();
    }
    void        SetLocalAffine(const Eigen::Affine3f& affine);

    void CopyTrans(const SceneNode& node) {
        m_translate = node.m_translate;
        m_scale     = node.m_scale;
        m_rotation  = node.m_rotation;
        m_alignmentOffset = node.m_alignmentOffset;
        MarkTransDirty();
    }

    bool Visible() const noexcept {
        // Effective visibility now combines the node-local flag with the layer-level flag that the
        // scene graph propagates. Keeping those concerns separate lets runtime-only support nodes,
        // such as effect internals, preserve their own local visibility contract without fighting
        // the generic layer visibility system every time a script or parent layer updates a layer.
        return m_localVisible && m_layerVisible &&
               (m_parent == nullptr || m_parent->Visible());
    }
    bool LocalVisible() const noexcept { return m_localVisible; }
    bool LayerVisible() const noexcept { return m_layerVisible; }
    void SetVisible(bool value) noexcept { SetLocalVisible(value); }
    void SetLocalVisible(bool value) noexcept { m_localVisible = value; }
    void SetLayerVisible(bool value) noexcept { m_layerVisible = value; }

    // update self modle trans (will update parent before)
    void            UpdateTrans();
    Eigen::Matrix4d ModelTrans() const { return m_trans; };

    SceneMesh* Mesh() { return m_mesh.get(); }
    const SceneMesh* Mesh() const { return m_mesh.get(); }
    // Callers use the text slot to decide whether a node should go through the dedicated text
    // pipeline or the traditional mesh/custom-shader pipeline. Returning raw pointers here keeps
    // the public API aligned with `Mesh()` while ownership still lives in `m_text`.
    SceneTextPrimitive* Text() { return m_text.get(); }
    const SceneTextPrimitive* Text() const { return m_text.get(); }
    bool       HasText() const { return m_text != nullptr; }
    bool       HasMaterial() const { return m_mesh && m_mesh->Material() != nullptr; };

    const auto& GetChildren() const { return m_children; }
    auto&       GetChildren() { return m_children; }
    SceneNode*  Parent() const noexcept { return m_parent; }

    i32& ID() { return m_id; }

private:
    // mark self and all children
    void MarkTransDirty();
    // Reparenting requires an unconditional invalidation pass. MarkTransDirty() is optimized for
    // ordinary local transform edits and skips recursion when the current node is already dirty,
    // which is exactly the wrong behavior for nodes whose descendants may still hold clean cached
    // matrices from the previous parent relationship.
    void MarkTransSubtreeDirty();

    i32         m_id;
    std::string m_name;

    bool            m_dirty;
    Eigen::Matrix4d m_trans;

    Eigen::Vector3f m_translate { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f m_scale { 1.0f, 1.0f, 1.0f };
    Eigen::Vector3f m_rotation { 0.0f, 0.0f, 0.0f };
    // Image alignment is part of the quad's local placement, not the authored layer origin.
    // Keeping it separate preserves Wallpaper Engine pivot semantics when scripts later animate
    // rotation, scale, parent attachment, or effect-composite synchronization for the same layer.
    Eigen::Vector3f m_alignmentOffset { 0.0f, 0.0f, 0.0f };

    std::shared_ptr<SceneMesh> m_mesh;
    // The text primitive holds canonical text geometry, atlas resources, and optional bridge
    // metadata. Storing it directly on the node is what makes text a scene-native renderable.
    std::shared_ptr<SceneTextPrimitive> m_text;
    bool m_localVisible { true };
    bool m_layerVisible { true };

    // specific a camera not active, used for image effect
    std::string m_cameraName;

    SceneNode* m_parent { nullptr };

    std::list<std::shared_ptr<SceneNode>> m_children;
};
} // namespace wallpaper
