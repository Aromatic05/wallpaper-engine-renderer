#pragma once
#include <list>
#include <vector>
#include <memory>
#include <string>
#include <Eigen/Dense>
#include <Eigen/Geometry>
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
    void        AddText(std::shared_ptr<SceneTextPrimitive> text) { m_text = std::move(text); }
    void        AppendChild(std::shared_ptr<SceneNode> sub) {
               sub->m_parent = this;
               m_children.push_back(sub);
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
    void        SetTranslate(Eigen::Vector3f v) {
        m_translate = v;
        MarkTransDirty();
    }
    void        SetScale(Eigen::Vector3f v) {
        m_scale = v;
        MarkTransDirty();
    }
    void        SetAlignmentOffset(Eigen::Vector3f v) {
        m_alignmentOffset = v;
        MarkTransDirty();
    }
    void        SetLocalAffine(const Eigen::Affine3f& affine);

    void CopyTrans(const SceneNode& node) {
        m_translate       = node.m_translate;
        m_scale           = node.m_scale;
        m_rotation        = node.m_rotation;
        m_alignmentOffset = node.m_alignmentOffset;
        MarkTransDirty();
    }

    // update self modle trans (will update parent before)
    void            UpdateTrans();
    Eigen::Matrix4d ModelTrans() const { return m_trans; };

    SceneMesh* Mesh() { return m_mesh.get(); }
    const SceneMesh* Mesh() const { return m_mesh.get(); }
    SceneTextPrimitive* Text() { return m_text.get(); }
    const SceneTextPrimitive* Text() const { return m_text.get(); }
    bool       HasText() const { return m_text != nullptr; }
    bool       HasMaterial() const { return m_mesh && m_mesh->Material() != nullptr; };

    const auto& GetChildren() const { return m_children; }
    auto&       GetChildren() { return m_children; }

    i32& ID() { return m_id; }

private:
    // mark self and all children
    void MarkTransDirty();

    i32         m_id;
    std::string m_name;

    bool            m_dirty;
    Eigen::Matrix4d m_trans;

    Eigen::Vector3f m_translate { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f m_scale { 1.0f, 1.0f, 1.0f };
    Eigen::Vector3f m_rotation { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f m_alignmentOffset { 0.0f, 0.0f, 0.0f };

    std::shared_ptr<SceneMesh> m_mesh;
    std::shared_ptr<SceneTextPrimitive> m_text;

    // specific a camera not active, used for image effect
    std::string m_cameraName;

    SceneNode* m_parent { nullptr };

    std::list<std::shared_ptr<SceneNode>> m_children;
};
} // namespace wallpaper
