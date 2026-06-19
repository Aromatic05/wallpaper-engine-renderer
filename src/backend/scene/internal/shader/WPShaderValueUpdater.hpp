#pragma once
#include <memory>
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>
#include <chrono>

#include <Eigen/Dense>

#include "core/Core.hpp"
#include "interface/IShaderValueUpdater.h"
#include "core/MapSet.hpp"
#include "scene/SpriteAnimation.hpp"
#include "animation/WPPuppet.hpp"

namespace wallpaper
{

class Scene;
class SceneNode;

struct WPUniformInfo {
    bool has_MI { false };
    bool has_M { false };
    bool has_AM { false };
    bool has_MVP { false };
    bool has_MVPI { false };
    bool has_ETVP { false };
    bool has_ETVPI { false };
    bool has_VP { false };

    bool has_BONES { false };
    bool has_TIME { false };
    bool has_DAYTIME { false };
    bool has_POINTERPOSITION { false };
    bool has_PARALLAXPOSITION { false };
    bool has_TEXELSIZE { false };
    bool has_TEXELSIZEHALF { false };
    bool has_SCREEN { false };
    bool has_LP { false };

    struct Tex {
        bool has_resolution { false };
        bool has_mipmap { false };
    };
    std::array<Tex, 12> texs;
};

struct WPShaderValueData {
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    // index + name
    std::vector<std::pair<usize, std::string>> renderTargets;

    WPPuppetLayer puppet_layer;
    SceneNode* parallax_anchor { nullptr };

    enum class TransformBindingMode
    {
        None,
        InheritParent,
        BoneAttachment,
    };

    struct TransformBinding {
        TransformBindingMode mode { TransformBindingMode::None };
        SceneNode*           parent { nullptr };
        uint32_t             bone_index { 0xFFFFFFFFu };
        Eigen::Affine3f      anchor_transform { Eigen::Affine3f::Identity() };
        Eigen::Affine3f      local_transform { Eigen::Affine3f::Identity() };
    };

    TransformBinding transform_binding {};
    bool             suppress_model_parallax { false };

    void SetParallaxAnchor(SceneNode* parent) { parallax_anchor = parent; }

    void SetParallaxContract(const std::array<float, 2>& depth,
                             SceneNode* anchor = nullptr,
                             bool suppress_own_model_parallax = false) {
        parallaxDepth = depth;
        parallax_anchor = anchor;
        suppress_model_parallax = suppress_own_model_parallax;
    }

    void SuppressOwnModelParallax() { suppress_model_parallax = true; }

    void CopyParallaxContractFrom(const WPShaderValueData& source) {
        SetParallaxContract(
            source.parallaxDepth, source.parallax_anchor, source.suppress_model_parallax);
    }

    void InheritParentTransform(SceneNode* parent, bool inherit_parent_parallax = true) {
        parallax_anchor = inherit_parent_parallax ? parent : nullptr;
        transform_binding.mode = TransformBindingMode::InheritParent;
        transform_binding.parent = parent;
    }

    void AttachToBone(SceneNode* parent,
                      uint32_t bone_index,
                      const Eigen::Affine3f& anchor_transform,
                      const Eigen::Affine3f& local_transform) {
        parallax_anchor = parent;
        transform_binding.mode = TransformBindingMode::BoneAttachment;
        transform_binding.parent = parent;
        transform_binding.bone_index = bone_index;
        transform_binding.anchor_transform = anchor_transform;
        transform_binding.local_transform = local_transform;
    }

    bool InheritsSceneParentTransform() const {
        return transform_binding.mode == TransformBindingMode::InheritParent;
    }

    bool IsBoneAttached() const {
        return transform_binding.mode == TransformBindingMode::BoneAttachment;
    }

    bool AppliesModelParallax() const {
        return !suppress_model_parallax && !IsBoneAttached();
    }

    SceneNode* TransformParent() const { return transform_binding.parent; }
};

struct WPCameraParallax {
    bool  enable { false };
    float amount;
    float delay;
    float mouseinfluence;
};

class WPShaderValueUpdater : public IShaderValueUpdater {
public:
    WPShaderValueUpdater(Scene* scene): m_scene(scene) {}
    virtual ~WPShaderValueUpdater() {}

    void FrameBegin() override;

    void InitUniforms(SceneNode*, const ExistsUniformOp&) override;
    void UpdateUniforms(SceneNode*, sprite_map_t&, const UpdateUniformOp&,
                        std::string_view cameraOverride = {}) override;
    void FrameEnd() override;
    void MouseInput(double, double) override;
    void SetTexelSize(float x, float y) override;

    void SetNodeData(void*, const WPShaderValueData&);
    const WPShaderValueData* GetNodeData(const void* node_addr) const;
    WPShaderValueData*       GetNodeData(const void* node_addr);
    void SetCameraParallax(const WPCameraParallax& value) {
        m_parallax = value;
        m_modelTransformCache.clear();
        m_parallaxOffsetCache.clear();
        m_attachmentTransformCache.clear();
    }

    void SetScreenSize(i32 w, i32 h) override { m_screen_size = { (float)w, (float)h }; }

private:
    Scene*               m_scene;
    WPCameraParallax     m_parallax;
    double               m_dayTime { 0.0f };
    std::array<float, 2> m_texelSize { 1.0f / 1920.0f, 1.0f / 1080.0f };

    std::array<float, 2> m_mousePos { 0.5f, 0.5f };
    std::array<float, 2> m_mousePosInput { 0.5f, 0.5f };
    double               m_mouseDelayedTime { 0.0f };
    uint                 m_mouseInputCount { 0 };

    std::chrono::time_point<std::chrono::steady_clock> m_last_mouse_input_time;

    std::array<float, 2> m_screen_size { 1920, 1080 };

    Map<void*, Eigen::Matrix4d>  m_modelTransformCache;
    Map<void*, Eigen::Vector3f>  m_parallaxOffsetCache;
    Map<void*, Eigen::Affine3f>  m_attachmentTransformCache;
    Map<void*, WPShaderValueData> m_nodeDataMap;
    Map<void*, WPUniformInfo>     m_nodeUniformInfoMap;
};
} // namespace wallpaper
