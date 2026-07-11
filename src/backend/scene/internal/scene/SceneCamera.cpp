#include "SceneCamera.h"
#include "SceneNode.h"
#include "utils/Logging.h"
#include "utils/Eigen.h"

#include <cmath>

using namespace wallpaper;
using namespace Eigen;

namespace
{
Matrix4d ResolveNodeCameraFrame(SceneNode& node) {
    node.UpdateTrans();

    Matrix4d frame = node.ModelTrans();
    if (! frame.allFinite()) return Matrix4d::Identity();

    constexpr double kAxisEpsilon = 1e-10;
    const Vector3d   x            = frame.block<3, 1>(0, 0);
    const Vector3d   y            = frame.block<3, 1>(0, 1);
    Vector3d         z            = frame.block<3, 1>(0, 2);

    if (! z.allFinite() || z.squaredNorm() <= kAxisEpsilon) {
        z = x.cross(y);
        if (! z.allFinite() || z.squaredNorm() <= kAxisEpsilon) {
            z = Vector3d::UnitZ();
        }
        frame.block<3, 1>(0, 2) = z.normalized();
    }

    if (! frame.allFinite() || std::abs(frame.determinant()) <= kAxisEpsilon) {
        return Matrix4d::Identity();
    }
    return frame;
}
} // namespace

Vector3d SceneCamera::GetPosition() const {
	if (m_hasExplicitView) {
		return m_explicitEye;
	}
	if(m_node) {
		return Affine3d(m_node->GetLocalTrans()) * Vector3d::Zero();
	}
	return Vector3d::Zero();
}

Vector3d SceneCamera::GetDirection() const {
	if (m_hasExplicitView) {
		// 3D camera paths author eye/center/up directly. Returning the explicit direction keeps
		// model-only camera uniforms synchronized with the same view matrix used for rendering.
		Vector3d direction = m_explicitCenter - m_explicitEye;
		if (direction.norm() > 1e-9) return direction.normalized();
		return -Vector3d::UnitZ();
	}
	if(m_node) {
		return (m_node->GetLocalTrans() * Vector4d(0.0f, 0.0f, -1.0f, 0.0f)).head<3>();
	}
	return -Vector3d::UnitZ();
}

Vector3d SceneCamera::GetUp() const {
	if (m_hasExplicitView) {
		if (m_explicitUp.norm() > 1e-9) return m_explicitUp.normalized();
	}
	return Vector3d::UnitY();
}

Matrix4d SceneCamera::GetViewMatrix() const {
	return m_viewMat;
}

Matrix4d SceneCamera::GetViewProjectionMatrix() const {
	return m_viewProjectionMat;
}

void SceneCamera::CalculateViewProjectionMatrix() {
	// CalculateViewMatrix
	{
		if (m_hasExplicitView) {
			// The model camera can be driven by Wallpaper Engine path keyframes without converting
			// through Euler scene-node state. This explicit branch is inert for 2D cameras because
			// only the model parser calls SetExplicitView().
			m_viewMat = LookAt(m_explicitEye, m_explicitCenter, m_explicitUp);
		} else if(m_node) {
			// A node camera is the inverse of the complete authored world frame. Using only a
			// LookAt derived from local translation drops parent transforms and scale, while directly
			// inverting a zero-scale node produces NaN/Inf. Repair a missing Z axis when possible and
			// otherwise fall back to an identity frame before inversion.
			m_viewMat = ResolveNodeCameraFrame(*m_node).inverse();
		} else
			m_viewMat = Matrix4d::Identity();
	};

	if(m_perspective) {
		m_viewProjectionMat = Perspective(Radians(m_fov), m_aspect, m_nearClip, m_farClip) * m_viewMat;
	} else {
		double left = -m_width/2.0f;
		double right = m_width/2.0f;
		double bottom = -m_height/2.0f;
		double up = m_height/2.0f;
		m_viewProjectionMat = Ortho(left, right, bottom, up, m_nearClip, m_farClip) * m_viewMat;
	}
}

void SceneCamera::Update() {
	CalculateViewProjectionMatrix();
}


void SceneCamera::AttatchNode(std::shared_ptr<SceneNode> node) {
	if(!node) {
		LOG_ERROR("Attach a null node to camera");		
		return;
	}
	m_node = node;
	Update();
}

void SceneCamera::SetExplicitView(const Eigen::Vector3d& eye,
                                  const Eigen::Vector3d& center,
                                  const Eigen::Vector3d& up) {
	m_explicitEye = eye;
	m_explicitCenter = center;
	m_explicitUp = up;
	m_hasExplicitView = true;
	Update();
}
