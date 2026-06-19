#include "SceneNode.h"

#include <Eigen/Geometry>

using namespace wallpaper;
using namespace Eigen;

Matrix4d SceneNode::GetLocalTrans() const {
    Affine3d trans = Affine3d::Identity();
    trans.prescale(m_scale.cast<double>());

    trans.prerotate(AngleAxis<double>(m_rotation.x(), Vector3d::UnitX())); // x
    trans.prerotate(AngleAxis<double>(m_rotation.y(), Vector3d::UnitY())); // y
    trans.prerotate(AngleAxis<double>(m_rotation.z(), Vector3d::UnitZ())); // z

    trans.pretranslate(m_translate.cast<double>());
    trans.translate(m_alignmentOffset.cast<double>());

    return trans.matrix();
}

void SceneNode::SetLocalAffine(const Affine3f& affine) {
    Affine3f authored_affine = affine;
    authored_affine.translate(-m_alignmentOffset);

    Matrix3f linear = authored_affine.linear();
    Vector3f scale(linear.col(0).norm(), linear.col(1).norm(), linear.col(2).norm());
    for (int i = 0; i < 3; ++i) {
        if (scale[i] > 1e-6f) {
            linear.col(i) /= scale[i];
        } else {
            linear.col(i).setZero();
            linear(i, i) = 1.0f;
            scale[i] = 1.0f;
        }
    }

    const auto zyx = linear.eulerAngles(2, 1, 0);
    SetScale(scale);
    SetRotation(Vector3f(zyx[2], zyx[1], zyx[0]));
    SetTranslate(authored_affine.translation());
}

void SceneNode::UpdateTrans() {
    if (! m_dirty) return;
    m_dirty = false;

    if (m_parent) {
        m_parent->UpdateTrans();
    }
    {
        Affine3d trans = Affine3d::Identity();
        if (m_parent) {
            trans *= m_parent->ModelTrans();
        }
        m_trans = (trans * GetLocalTrans()).matrix();
    }
}

void SceneNode::MarkTransDirty() {
    if (! m_dirty) {
        m_dirty = true;
        for (auto& child : m_children) {
            child->MarkTransDirty();
        }
    }
}
