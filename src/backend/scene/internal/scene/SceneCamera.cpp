#include "SceneCamera.h"
#include "SceneNode.h"
#include "utils/Logging.h"
#include <iostream>
#include "utils/Eigen.h"

using namespace wallpaper;
using namespace Eigen;

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
			Affine3d nodeTrans(m_node->GetLocalTrans());
			Vector3d eye = nodeTrans * Vector3d::Zero();
			Vector3d center = nodeTrans * (-Vector3d::UnitZ());
			Vector3d up = Vector3d::UnitY();
			m_viewMat = LookAt(eye, center, up);
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
