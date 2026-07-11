#pragma once
#include <vector>
#include <list>
#include <memory>
#include <cstdint>
#include <string>
#include <Eigen/Geometry>
#include "core/Literals.hpp"
#include "Type.hpp"

namespace wallpaper
{

class SceneNode;
class SceneMesh;

struct SceneImageEffectNode {
    // Authored effect passes use symbolic ping-pong targets. ResolveEffect() maps those symbols to
    // concrete render targets every time the graph is built, so keep the parsed template separate
    // from the mutable runtime output to avoid carrying one build's A/B swap into the next build.
    std::string                authored_output; // parsed render target template
    std::string                output;          // resolved render target for the current graph
    std::vector<std::string>   authored_textures;
    std::shared_ptr<SceneNode> sceneNode;
    // Effect nodes are render-graph internals, not authored scene owners. When an internal pass
    // needs a layer-local camera, store it as a pass override instead of assigning it to
    // SceneNode::Camera(); otherwise graph traversal may see the image-effect camera attached to
    // that layer and recursively resolve the same effect chain a second time.
    std::string camera_override;
    bool        use_active_camera_for_parallax { false };
    bool        clear_before_draw { false };
    bool        force_alpha_write { false };
    // Some effect chains end with a synthetic layer-surface writer. Animated puppet images with
    // authored effects are the important case: intermediate shaders run as fullscreen private
    // passes, then the final synthetic material samples that result through the puppet mesh so
    // skinning, blinking, and attachment motion are still applied. If a composition-layer route
    // keeps this final pass private, it must render through the layer's own source camera and
    // authored final mesh instead of being collapsed to the generic 2x2 effect quad.
    bool private_final_output_uses_layer_surface { false };
};

struct SceneImageEffect {
    enum class CmdType
    {
        Copy,
    };
    struct Command {
        CmdType     cmd { CmdType::Copy };
        // Copy commands may refer to the current input ping-pong target through the same symbolic
        // names as shader passes. The runtime src/dst below are regenerated from these parsed values
        // on every ResolveEffect() call.
        std::string authored_dst;
        std::string authored_src;
        std::string dst;
        std::string src;
        i32         afterpos { 0 }; // start at 1, 0 for begin at all
    };
    std::vector<Command>            commands;
    std::list<SceneImageEffectNode> nodes;

    // Effect visibility is a first-class runtime contract. Wallpaper Engine allows an effect to
    // start hidden and later become visible through a script, user property, or animation. The
    // effect therefore needs its own local visibility state instead of borrowing the owner layer's
    // visibility or being pruned while parsing.
    void SetIdentity(int32_t owner_layer_id, int32_t effect_id, uint32_t effect_index,
                     std::string effect_name);
    void SetRuntimeVisibilityContract(bool runtime_visibility_contract) {
        // Runtime-driven effect visibility is a render-topology contract, not only a property
        // value. A final effect that can disappear while the graph stays alive must not be the
        // persistent `_rt_default` writer, because skipping that pass can leave the previous frame
        // inside feedback copies. SceneImageEffectLayer uses this flag to publish such final
        // effects through the neutral final composite instead.
        m_runtime_visibility_contract = runtime_visibility_contract;
    }
    void SetLocalVisible(bool visible);
    bool LocalVisible() const { return m_local_visible; }
    bool HasRuntimeVisibilityContract() const { return m_runtime_visibility_contract; }

    int32_t            OwnerLayerId() const { return m_owner_layer_id; }
    int32_t            EffectId() const { return m_effect_id; }
    uint32_t           EffectIndex() const { return m_effect_index; }
    const std::string& EffectName() const { return m_effect_name; }

    // ResolveEffect() rewrites authored ping-pong aliases to concrete render targets. These two
    // names describe the stable bypass copy used while the effect is hidden: copy the input
    // ping-pong target to the output ping-pong target so downstream effects never sample a stale
    // frame left by the last visible execution.
    void SetBypassTargets(std::string src, std::string dst);
    const std::string& BypassSource() const { return m_bypass_src; }
    const std::string& BypassTarget() const { return m_bypass_dst; }

private:
    int32_t     m_owner_layer_id { 0 };
    int32_t     m_effect_id { 0 };
    uint32_t    m_effect_index { 0 };
    std::string m_effect_name;
    bool        m_runtime_visibility_contract { false };
    bool        m_local_visible { true };
    std::string m_bypass_src;
    std::string m_bypass_dst;
};

class SceneImageEffectLayer {
public:
    enum class HiddenFinalCompositePolicy
    {
        PreserveSource,
        SuppressOutput,
    };
    enum class SourcePolicy
    {
        OwnerNode,
        OwnerNodeAndProxyChildren,
        ProxyChildrenOnly,
    };
    enum class FinalOutputPolicy
    {
        AuthoredWriter,
        PrivateAuthoredThenComposite,
    };

    SceneImageEffectLayer(SceneNode* node, float w, float h, std::string_view pingpong_a,
                          std::string_view pingpong_b);

    void AddEffect(const std::shared_ptr<SceneImageEffect>& node) { m_effects.push_back(node); }
    void AddPrefillNode(SceneImageEffectNode node) { m_prefill_nodes.push_back(std::move(node)); }
    const std::vector<SceneImageEffectNode>& PrefillNodes() const { return m_prefill_nodes; }
    std::size_t EffectCount() const { return m_effects.size(); }
    auto&       GetEffect(std::size_t index) { return m_effects.at(index); }
    const auto& FirstTarget() const { return m_pingpong_a; }
    SceneMesh&  SourceMesh() const { return *m_source_mesh; }
    SceneMesh&  FinalMesh() const { return *m_final_mesh; }
    SceneNode&  FinalNode() const { return *m_final_node; }
    SourcePolicy SourceContributionPolicy() const { return m_source_policy; }
    bool        HasFinalComposite() const;
    bool        ShouldRunFinalComposite() const;
    bool        PublishesPrivateFinalComposite() const {
        return m_final_composite.publishes_private_output;
    }
    bool        FinalCompositeSamplesPremultipliedSource() const {
        return m_final_composite.samples_premultiplied_source;
    }
    bool        ClearSourceBeforeOwnerDraw() const { return m_clear_source_before_owner_draw; }
    void        SetFinalCompositeSource(std::string source);
    void        SetFullscreen(bool fullscreen) { m_fullscreen = fullscreen; }
    void        SetSourceContributionPolicy(SourcePolicy policy) { m_source_policy = policy; }
    void        SetClearSourceBeforeOwnerDraw(bool clear_source_before_owner_draw) {
        m_clear_source_before_owner_draw = clear_source_before_owner_draw;
    }
    void        SetHiddenFinalCompositePolicy(HiddenFinalCompositePolicy policy) {
        // Hidden final effects have two valid source contracts. Ordinary images/text preserve the
        // pre-effect source when an effect is disabled. Source-less passthrough/compose helpers must
        // instead contribute nothing, because preserving their uninitialized helper target can draw
        // stale framebuffer-sized quads while the authored effect is hidden.
        m_final_composite.hidden_policy = policy;
    }
    SceneNode*  WorldNode() const { return m_worldNode; }
    void        SetFinalBlend(BlendMode m) { m_final_blend = m; }
    void        SyncResolvedOutputMesh();
    void        SyncResolvedNodeToWorld();
    void        SyncResolvedNodeToMatrix(const Eigen::Affine3f& world_affine);
    const std::string& ResolvedPrivateOutputTarget() const {
        return m_resolved_private_output_target;
    }

    void ResolveEffect(const SceneMesh& defualt_mesh, std::string_view effect_cam,
                       std::string_view layer_surface_cam,
                       std::string_view final_output,
                       bool keep_final_output_private = false,
                       const Eigen::Affine3f* resolved_world_affine = nullptr,
                       FinalOutputPolicy final_output_policy = FinalOutputPolicy::AuthoredWriter);

private:
    struct FinalCompositeState {
        SceneImageEffect* output_effect { nullptr };
        // Source-less helper chains promote the neutral final composite to the stable visible
        // publisher. Private composition-source routes use the same neutral pass differently: the
        // authored final pass stays private, then the neutral pass publishes that resolved texture
        // into the parent compose source. Keeping both publication modes in one state object makes
        // the final-output state machine explicit without scattering interdependent booleans across
        // the layer.
        bool publishes_visible_output { false };
        bool publishes_private_output { false };
        bool uses_source_mesh { false };
        // Private layer-surface puppet writers blend their straight-alpha samples into a transparent
        // local render target before the neutral final composite publishes that target. That
        // offscreen image therefore carries premultiplied RGB plus source-over coverage alpha; the
        // publisher must not multiply RGB by alpha a second time.
        bool samples_premultiplied_source { false };
        HiddenFinalCompositePolicy hidden_policy { HiddenFinalCompositePolicy::PreserveSource };

        void ResetForResolve() {
            output_effect = nullptr;
            publishes_visible_output = false;
            publishes_private_output = false;
            uses_source_mesh = false;
            samples_premultiplied_source = false;
        }
    };

    struct FinalOutputResolveDecision {
        bool keep_authored_final_private { false };
        bool private_final_uses_layer_surface { false };
    };

    SceneNode*  m_worldNode;
    std::string m_pingpong_a;
    std::string m_pingpong_b;

    // Fullscreen utility layers, such as Wallpaper Engine's postprocess framebuffer layer, are
    // authored in clip-space sized 2x2 quads. Their final effect pass must therefore stay on the
    // effect camera/fullscreen mesh path; resolving that pass through the active scene camera turns
    // a shader such as godrays_combine into a tiny world-space quad and makes the rays disappear.
    bool m_fullscreen { false };
    SourcePolicy m_source_policy { SourcePolicy::OwnerNode };
    //    std::vector<float> m_size;
    std::unique_ptr<SceneMesh> m_source_mesh;
    std::unique_ptr<SceneMesh> m_final_mesh;
    std::unique_ptr<SceneNode> m_final_node;
    SceneNode*                 m_resolved_output_node { nullptr };
    // Visible effect layers resolve their final authored pass in scene space, so runtime transform
    // updates must keep that node synchronized with the layer world node. Hidden dependency sources
    // resolve into a private offscreen texture instead; their final pass must stay in the effect
    // camera's local fullscreen space or `_rt_imageLayerComposite_<id>` samples a shifted source.
    bool                       m_resolved_output_follows_world { true };
    bool                       m_resolved_output_mesh_follows_final_mesh { true };
    std::string                m_resolved_private_output_target;
    bool                       m_clear_source_before_owner_draw { false };
    FinalCompositeState        m_final_composite;
    BlendMode                  m_final_blend;

    std::vector<SceneImageEffectNode> m_prefill_nodes;
    std::vector<std::shared_ptr<SceneImageEffect>> m_effects;

    bool HasRuntimeVisibilityContract() const;
    bool HasVisibleRuntimeVisibilityContribution() const;
    bool HasVisibleSourceLessContribution() const;
    void SyncResolvedNodeForRoute(const Eigen::Affine3f* resolved_world_affine);
    SceneImageEffectNode* ResolveEffectPingPongChain(const SceneMesh& default_mesh,
                                                     SceneNode& default_node,
                                                     std::string_view effect_cam,
                                                     std::string_view& ppong_a,
                                                     std::string_view& ppong_b);
    FinalOutputResolveDecision ResolveFinalOutputDecision(
        SceneImageEffectNode* fallback_last_output,
        std::string_view layer_surface_cam,
        bool keep_final_output_private,
        FinalOutputPolicy final_output_policy);
    void ResolveFinalCompositeNode(const SceneMesh& default_mesh,
                                   SceneNode& default_node,
                                   std::string_view effect_cam,
                                   std::string_view final_output,
                                   std::string_view final_composite_source,
                                   const Eigen::Affine3f* resolved_world_affine);
    void ResolveVisibleFinalOutput(SceneImageEffectNode& final_output_node,
                                   const SceneMesh& default_mesh,
                                   SceneNode& default_node,
                                   std::string_view effect_cam,
                                   std::string_view final_output,
                                   const Eigen::Affine3f* resolved_world_affine);
    void ResolvePrivateFinalOutput(SceneImageEffectNode& final_output_node,
                                   const SceneMesh& default_mesh,
                                   SceneNode& default_node,
                                   std::string_view effect_cam,
                                   std::string_view layer_surface_cam,
                                   bool private_final_uses_layer_surface,
                                   const Eigen::Affine3f* resolved_world_affine);
};
} // namespace wallpaper
