#pragma once
#include <memory>
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>

#include <Eigen/Dense>

#include "core/Core.hpp"
#include "interface/IShaderValueUpdater.h"
#include "core/MapSet.hpp"
#include "SpriteAnimation.hpp"
#include "WPPuppet.hpp"

namespace wallpaper
{

class Scene;
class SceneNode;
class SceneMesh;

struct WPUniformInfo {
    bool has_MI { false };
    bool has_M { false };
    bool has_AM { false };
    bool has_MVP { false };
    // Wallpaper Engine effect passes can render through helper meshes while shader math still
    // depends on the authored layer transform. Track these matrix uniforms separately so effect
    // shaders get that contract without requiring a g_ModelViewProjectionMatrix declaration too.
    bool has_LMM { false };
    bool has_EMVP { false };
    bool has_MVPI { false };
    bool has_ETVP { false };
    bool has_ETVPI { false };
    bool has_VP { false };

    bool has_BONES { false };
    bool has_TIME { false };
    bool has_DAYTIME { false };
    // Cursor feedback shaders need these values as a coherent per-frame set. Tracking them beside
    // the older pointer position bit keeps UpdateUniforms data-driven: ordinary materials do not pay
    // for cursor state writes, while effects like cursorripple receive every uniform they declare.
    bool has_FRAMETIME { false };
    bool has_POINTERPOSITION { false };
    bool has_POINTERPOSITIONLAST { false };
    bool has_POINTERSTATE { false };
    bool has_PARALLAXPOSITION { false };
    bool has_TEXELSIZE { false };
    bool has_TEXELSIZEHALF { false };
    bool has_SCREEN { false };
    bool has_LP { false };
    // These extra light payloads and the camera basis uniforms are part of the 3D model shader
    // contract. InitUniforms enables them only on materialized model nodes so 2D shaders keep their
    // previous uniform surface even if they happen to declare similarly named values.
    bool has_model_LCP { false };
    bool has_LCR { false };
    bool has_EYE_POSITION { false };
    bool has_VIEWUP { false };
    bool has_VIEWRIGHT { false };
    bool has_VIEWFORWARD { false };
    std::array<bool, 3> has_audio_spectrum_left { false, false, false };
    std::array<bool, 3> has_audio_spectrum_right { false, false, false };

    struct Tex {
        bool has_resolution { false };
        bool has_mipmap { false };
    };
    std::array<Tex, 12> texs;
};

enum class WPNodeTransformBindingMode
{
    None,
    InheritParent,
    BoneAttachment,
};

struct WPNodeTransformBinding {
    WPNodeTransformBindingMode mode { WPNodeTransformBindingMode::None };
    SceneNode*                 parent { nullptr };
    uint32_t                   bone_index { 0xFFFFFFFFu };
    Eigen::Affine3f            anchor_transform { Eigen::Affine3f::Identity() };
    Eigen::Affine3f            local_transform { Eigen::Affine3f::Identity() };

    bool InheritsParentTransform() const {
        return mode == WPNodeTransformBindingMode::InheritParent;
    }

    bool IsBoneAttachment() const {
        return mode == WPNodeTransformBindingMode::BoneAttachment;
    }
};

struct WPShaderValueData {
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    // index + name
    std::vector<std::pair<usize, std::string>> renderTargets;

    WPPuppetLayer puppet_layer;
    SceneNode*     parallax_anchor { nullptr };
    WPNodeTransformBinding transform_binding {};
    SceneNode*            effect_projection_node { nullptr };
    SceneMesh*            effect_projection_mesh { nullptr };
    // Some repaired Wallpaper Engine container nodes must provide the canonical mouse-parallax
    // offset for their authored children without translating their own final offscreen composite.
    // Moving that full render-target quad exposes rectangular background seams when the target
    // contains cleared/filtered pixels around the character, while child nodes still need to anchor
    // to the exact same parallax source to remain synchronized.
    bool                  suppress_model_parallax { false };

    void SetParallaxAnchor(SceneNode* parent) { parallax_anchor = parent; }

    void SetParallaxContract(const std::array<float, 2>& depth, SceneNode* anchor = nullptr,
                             bool suppress_own_model_parallax = false) {
        parallaxDepth           = depth;
        parallax_anchor         = anchor;
        suppress_model_parallax = suppress_own_model_parallax;
    }

    void SetEffectProjection(SceneNode* projection_node, SceneMesh* projection_mesh) {
        effect_projection_node = projection_node;
        effect_projection_mesh = projection_mesh;
    }

    void SuppressOwnModelParallax() { suppress_model_parallax = true; }

    void CopyParallaxContractFrom(const WPShaderValueData& source) {
        SetParallaxContract(
            source.parallaxDepth, source.parallax_anchor, source.suppress_model_parallax);
    }

    void InheritParentTransform(SceneNode* parent, bool inherit_parent_parallax = true) {
        parallax_anchor        = inherit_parent_parallax ? parent : nullptr;
        transform_binding.mode = WPNodeTransformBindingMode::InheritParent;
        transform_binding.parent = parent;
    }

    void AttachToBone(SceneNode* parent, uint32_t bone_index,
                      const Eigen::Affine3f& anchor_transform,
                      const Eigen::Affine3f& local_transform) {
        parallax_anchor                    = parent;
        transform_binding.mode             = WPNodeTransformBindingMode::BoneAttachment;
        transform_binding.parent           = parent;
        transform_binding.bone_index       = bone_index;
        transform_binding.anchor_transform = anchor_transform;
        transform_binding.local_transform  = local_transform;
    }

    bool InheritsSceneParentTransform() const {
        return transform_binding.InheritsParentTransform();
    }

    bool IsBoneAttached() const { return transform_binding.IsBoneAttachment(); }

    bool AppliesModelParallax() const {
        return ! suppress_model_parallax && ! transform_binding.IsBoneAttachment();
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
                        const ShaderUniformOverrides* overrides = nullptr) override;
    void FrameEnd() override;
    void MouseInput(double, double) override;
    void SetTexelSize(float x, float y) override;

    void SetNodeData(void*, const WPShaderValueData&);
    const WPShaderValueData* GetNodeData(const void* node_addr) const;
    WPShaderValueData*       GetNodeData(const void* node_addr);
    void ReplaceNodeReferences(SceneNode* old_node, SceneNode* new_node);
    void SetCameraParallax(const WPCameraParallax& value) {
        m_parallax = value;
        // Camera parallax changes alter the derived model transforms even when the authored layer
        // transform data is unchanged. Clear the per-frame caches immediately so a runtime toggle
        // cannot leave puppet/model layers using offsets computed with the previous global state.
        m_modelTransformCache.clear();
        m_parallaxOffsetCache.clear();
        m_attachmentTransformCache.clear();
    }
    void     AdvanceAllPuppets();
    uint64_t NextPuppetFrameSerial() const noexcept { return m_puppet_frame_serial + 1; }

    void SetScreenSize(i32 w, i32 h) override { m_screen_size = { (float)w, (float)h }; }

private:
    Scene*               m_scene;
    WPCameraParallax     m_parallax;
    double               m_dayTime { 0.0f };
    std::array<float, 2> m_texelSize { 1.0f / 1920.0f, 1.0f / 1080.0f };

    std::array<float, 2> m_mousePos { 0.5f, 0.5f };
    // Stores the position published on the previous FrameBegin. Feedback effects compare this with
    // g_PointerPosition to decide whether to inject a new cursor impulse into their simulation FBOs.
    std::array<float, 2> m_mousePosLast { 0.5f, 0.5f };
    std::array<float, 2> m_mousePosInput { 0.5f, 0.5f };

    std::array<float, 2> m_screen_size { 1920, 1080 };

    uint64_t                     m_puppet_frame_serial { 0 };
    Map<void*, Eigen::Matrix4d>  m_modelTransformCache;
    Map<void*, Eigen::Vector3f>  m_parallaxOffsetCache;
    Map<void*, Eigen::Affine3f>  m_attachmentTransformCache;
    Map<void*, WPShaderValueData> m_nodeDataMap;
    Map<void*, WPUniformInfo>     m_nodeUniformInfoMap;
};
} // namespace wallpaper
