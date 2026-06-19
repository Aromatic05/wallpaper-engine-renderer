#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <Eigen/Dense>
#include "SceneImageEffectLayer.h"

namespace wallpaper
{

class SceneNode;

class SceneCamera {
public:
    explicit SceneCamera(i32 width, i32 height, float near, float far)
        : m_width(width),
          m_height(height),
          m_aspect(m_width / m_height),
          m_nearClip(near),
          m_farClip(far),
          m_perspective(false) {}

    explicit SceneCamera(float aspect, float near, float far, float fov)
        : m_aspect(aspect), m_nearClip(near), m_farClip(far), m_fov(fov), m_perspective(true) {}

    SceneCamera(const SceneCamera&) = default;

    void Update();

    void AttatchNode(std::shared_ptr<SceneNode>);

    bool   IsPerspective() const { return m_perspective; }
    double Aspect() const { return m_aspect; }
    double Width() const { return m_width; }
    double Height() const { return m_height; }
    double NearClip() const { return m_nearClip; }
    double FarClip() const { return m_farClip; }
    double Fov() const { return m_fov; }

    void SetWidth(double value) {
        m_width  = value;
        m_aspect = m_width / m_height;
    }
    void SetHeight(double value) {
        m_height = value;
        m_aspect = m_width / m_height;
    }
    void SetAspect(double aspect) { m_aspect = aspect; }
    void SetFov(double value) { m_fov = value; }
    void SetNearClip(double value) { m_nearClip = value; }
    void SetFarClip(double value) { m_farClip = value; }
    void SetExplicitView(const Eigen::Vector3d& eye,
                         const Eigen::Vector3d& center,
                         const Eigen::Vector3d& up);
    void ClearExplicitView() { m_hasExplicitView = false; }

    void  AttatchImgEffect(std::shared_ptr<SceneImageEffectLayer> eff) { m_imgEffect = eff; }
    bool  HasImgEffect() const { return (bool)m_imgEffect; }
    auto& GetImgEffect() { return m_imgEffect; }

    Eigen::Vector3d GetPosition() const;
    Eigen::Vector3d GetDirection() const;
    Eigen::Vector3d GetUp() const;

    Eigen::Matrix4d GetViewMatrix() const;
    Eigen::Matrix4d GetViewProjectionMatrix() const;

    std::shared_ptr<SceneNode> GetAttachedNode() const { return m_node; }

    void Clone(const SceneCamera& cam) {
        m_width       = cam.m_width;
        m_height      = cam.m_height;
        m_aspect      = cam.m_aspect;
        m_nearClip    = cam.m_nearClip;
        m_farClip     = cam.m_farClip;
        m_perspective = cam.m_perspective;
        m_hasExplicitView = cam.m_hasExplicitView;
        m_explicitEye = cam.m_explicitEye;
        m_explicitCenter = cam.m_explicitCenter;
        m_explicitUp = cam.m_explicitUp;
    }

private:
    void CalculateViewProjectionMatrix();

    double m_width { 1.0f };
    double m_height { 1.0f };
    double m_aspect { 16.0f / 9.0f };
    double m_nearClip { 0.01f };
    double m_farClip { 1000.0f };
    double m_fov { 45.0f };
    bool   m_perspective;
    bool   m_hasExplicitView { false };
    Eigen::Vector3d m_explicitEye { Eigen::Vector3d::Zero() };
    Eigen::Vector3d m_explicitCenter { -Eigen::Vector3d::UnitZ() };
    Eigen::Vector3d m_explicitUp { Eigen::Vector3d::UnitY() };

    Eigen::Matrix4d m_viewMat { Eigen::Matrix4d::Identity() };
    Eigen::Matrix4d m_viewProjectionMat { Eigen::Matrix4d::Identity() };

    std::shared_ptr<SceneNode>             m_node;
    std::shared_ptr<SceneImageEffectLayer> m_imgEffect { nullptr };
};
} // namespace wallpaper
