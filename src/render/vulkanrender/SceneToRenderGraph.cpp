#include "SceneToRenderGraph.hpp"

#include "scene/Scene.h"
#include "rendergraph/RenderGraph.hpp"
#include "SpecTexs.hpp"
#include "core/MapSet.hpp"
#include "utils/Logging.h"
#include "transform/WPImageAlignment.hpp"

#include "AllPasses.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace wallpaper;
namespace wallpaper::rg
{

void doCopy(RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc, TexNode* in, TexNode* out) {
    builder.read(in);
    builder.write(out);

    desc.src = in->key();
    desc.dst = out->key();
}
void addCopyPass(RenderGraph& rgraph, TexNode* in, TexNode* out,
                 std::function<bool()> should_execute = {}) {
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [in, out, should_execute = std::move(should_execute)](
            RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc) {
            doCopy(builder, desc, in, out);
            desc.should_execute = should_execute;
        });
}

void addCopyPass(RenderGraph& rgraph, const TexNode::Desc& in, const TexNode::Desc& out,
                 std::function<bool()> should_execute = {}) {
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [in, out, should_execute = std::move(should_execute)](
            RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc) {
            auto* in_node  = builder.createTexNode(in);
            auto* out_node = builder.createTexNode(out, true);
            doCopy(builder, desc, in_node, out_node);
            desc.should_execute = should_execute;
        });
}

TexNode* addCopyPass(RenderGraph& rgraph, TexNode* in, TexNode::Desc* out_desc = nullptr,
                     std::function<bool()> should_execute = {}) {
    TexNode* copy { nullptr };
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [&copy, in, out_desc, should_execute = std::move(should_execute)](
            RenderGraphBuilder& builder, vulkan::CopyPass::Desc& pdesc) {
            auto desc = out_desc == nullptr ? in->genDesc() : *out_desc;
            if (out_desc == nullptr) {
                desc.key += "_" + std::to_string(in->version()) + "_copy";
                desc.name += "_" + std::to_string(in->version()) + "_copy";
            }
            copy = builder.createTexNode(desc, true);
            doCopy(builder, pdesc, in, copy);
            pdesc.should_execute = should_execute;
            pdesc.track_source_extent = true;
        });
    return copy;
}

void addClearPass(RenderGraph& rgraph, const TexNode::Desc& target,
                  std::array<float, 4> color = { 0.0f, 0.0f, 0.0f, 0.0f }) {
    rgraph.addPass<vulkan::ClearPass>(
        "clear",
        PassNode::Type::Clear,
        [target, color](RenderGraphBuilder& builder, vulkan::ClearPass::Desc& desc) {
            auto* target_node = builder.createTexNode(target, true);
            builder.write(target_node);
            desc.target      = target_node->key();
            desc.clear_value = VkClearValue { .color = { color[0], color[1], color[2], color[3] } };
        });
}

static bool IsRuntimeRenderTarget(const Scene* scene, const std::string& path) {
    // Authored effect FBO names are not guaranteed to carry Wallpaper Engine's `_rt_` prefix after
    // the parser uniquifies them with the effect-layer address. The render-target table is the
    // authoritative runtime contract, so graph construction must consult it before classifying a
    // texture edge as an imported asset.
    return IsSpecTex(path) || (scene != nullptr && scene->renderTargets.count(path) != 0);
}

static TexNode::Desc createTexDesc(std::string path, const Scene* scene = nullptr) {
    return TexNode::Desc { .name = path,
                           .key  = path,
                           .type = IsRuntimeRenderTarget(scene, path) ? TexNode::TexType::Temp
                                                                      : TexNode::TexType::Imported };
}
} // namespace wallpaper::rg

static void CheckAndSetSprite(Scene& scene, vulkan::CustomShaderPass::Desc& desc,
                              std::span<const std::string> texs) {
    for (usize i = 0; i < texs.size(); i++) {
        auto& tex = texs[i];
        if (! tex.empty() && ! IsSpecTex(tex) && scene.textures.count(tex) != 0) {
            const auto& stex = scene.textures.at(tex);
            if (stex.isSprite) {
                desc.sprites_map[i] = stex.spriteAnim;
            }
        }
    }
}

static bool ShouldExecuteHiddenDependency(Scene& scene, SceneNode* node, std::string_view output) {
    const auto owner_it = scene.nodeOwners.find(node);
    if (owner_it == scene.nodeOwners.end()) return false;
    if (scene.offscreenDependencyLayerIds.count(owner_it->second) == 0) return false;

    // Hidden dependency layers are allowed to keep rendering only into private offscreen targets that
    // another effect samples. They must never use that exemption for `_rt_default`, because that
    // would make an invisible helper layer composite directly onto the wallpaper and create the large
    // tinted rectangles seen in xray-style scenes.
    return output != SpecTex_Default;
}

struct DelayLinkInfo {
    rg::NodeID id;
    rg::NodeID link_id;
    i32        tex_index;
};

struct ExtraInfo {
    Map<size_t, rg::TexNode*>  id_link_map {};
    std::vector<DelayLinkInfo> link_info {};
    rg::RenderGraph*           rgraph { nullptr };
    Scene*                     scene { nullptr };
    std::unordered_map<int32_t, size_t> layer_order_index {};
    // Model depth is shared per output target. Tracking the first model pass here lets the graph
    // clear depth once for each target, then load it for later chunks without touching 2D passes.
    std::unordered_set<std::string> model_depth_outputs_seen {};
    bool                       include_hidden_for_pipeline_warmup { false };
};

static rg::TexNode* AddMipFramebufferCopy(rg::RenderGraph&        rgraph,
                                          rg::RenderGraphBuilder& builder) {
    auto* source = builder.createTexNode(rg::TexNode::Desc { .name = SpecTex_Default.data(),
                                                             .key  = SpecTex_Default.data(),
                                                             .type = rg::TexNode::TexType::Temp });
    auto copy_desc = rg::TexNode::Desc { .name = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                         .key  = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                         .type = rg::TexNode::TexType::Temp };
    return rg::addCopyPass(rgraph, source, &copy_desc);
}

static bool IsOffscreenDependencyLayer(const ExtraInfo& extra, i32 imgId) {
    return extra.scene != nullptr && imgId != 0 &&
        extra.scene->offscreenDependencyLayerIds.count(imgId) != 0;
}

static bool ShouldPublishLayerLinkOutput(const ExtraInfo& extra, i32 imgId,
                                         std::string_view output) {
    if (!IsOffscreenDependencyLayer(extra, imgId)) return true;
    if (output != SpecTex_Default) return true;

    // `_rt_imageLayerComposite_<id>` is a source-texture contract, not a screen-composite contract.
    // Hidden dependency layers may still contain historical final passes that target `_rt_default`,
    // but those passes are deliberately blocked from executing while the layer is invisible. Letting
    // such a skipped screen pass replace the layer's link source makes the consumer sample the
    // wallpaper/default target instead of the dependency's raw or effect-resolved offscreen image.
    return false;
}

static bool ShouldKeepEffectFinalOutputPrivate(const ExtraInfo& extra, SceneNode* node, i32 imgId,
                                               std::string_view inherited_output) {
    if (!IsOffscreenDependencyLayer(extra, imgId)) return false;

    const bool visible_default_route =
        node != nullptr && node->Visible() && inherited_output == SpecTex_Default;
    // A layer can be both an offscreen dependency source and a normal visible wallpaper layer. The
    // dependency route needs the final authored effect to stay in ping-pong space so
    // `_rt_imageLayerComposite_<id>` samples a resolved private texture. The visible default route is
    // the opposite contract: the authored final effect is the screen writer, while the synthetic
    // fallback is gated off whenever that final effect is visible. Keeping this visible route private
    // leaves `_rt_default` untouched and produces the gray frame reported by Rika/943626357.
    return !visible_default_route;
}

struct OrderedRenderGraphChild {
    SceneNode* node { nullptr };
    bool       proxy { false };
    size_t     sequence { 0 };
};

struct NodePassOptions {
    bool        force_alpha_write { false };
    bool        clear_before_draw { false };
    std::string camera_override;
    bool        use_active_camera_for_parallax { false };
    bool        premultiplied_source_blend { false };
    bool        use_active_camera_for_uniforms { false };
};

struct TraversalRoute {
    bool                           routed_node { false };
    std::optional<Eigen::Matrix4d> model;
    bool                           compose_source { false };
    std::string                    compose_source_camera;
    bool                           premultiplied_source_blend { false };
};

static bool HasRenderableMeshMaterial(SceneNode* node) {
    return node != nullptr && node->Mesh() != nullptr && node->Mesh()->Material() != nullptr;
}

static int32_t NodeLayerId(const Scene& scene, SceneNode* node) {
    if (node == nullptr) return 0;
    if (auto owner_it = scene.nodeOwners.find(node); owner_it != scene.nodeOwners.end()) {
        return owner_it->second;
    }
    return node->ID();
}

static bool ShouldEmitLayerNodeForResidency(Scene& scene, SceneNode* node, const ExtraInfo& extra) {
    if (node == nullptr || node == scene.sceneGraph.get()) return true;
    if (extra.include_hidden_for_pipeline_warmup) return true;

    const int32_t layer_id = NodeLayerId(scene, node);
    if (layer_id == 0) return true;
    if (scene.IsLayerVisible(layer_id)) return true;

    // Dependency-source layers are the one hidden case that must stay resident in the graph: other
    // visible effects can sample their private offscreen outputs. Ordinary hidden layers are
    // pruned so their passes, framebuffers, descriptors, imported textures, and video decoders can
    // be released until the layer becomes visible again.
    return scene.offscreenDependencyLayerIds.count(layer_id) != 0;
}

static size_t NodeLayerOrderIndex(SceneNode* node, const ExtraInfo& extra) {
    if (extra.scene == nullptr || node == nullptr) return std::numeric_limits<size_t>::max();
    const int32_t layer_id = NodeLayerId(*extra.scene, node);
    if (auto it = extra.layer_order_index.find(layer_id); it != extra.layer_order_index.end()) {
        return it->second;
    }
    return std::numeric_limits<size_t>::max();
}

static bool IsEffectLocalProxyDependency(SceneNode* node, const ExtraInfo& extra) {
    if (extra.scene == nullptr || node == nullptr) return false;
    const int32_t layer_id = NodeLayerId(*extra.scene, node);
    return layer_id != 0 && extra.scene->offscreenDependencyLayerIds.count(layer_id) != 0;
}

static bool EffectSourceUsesProxyChildren(SceneImageEffectLayer* imgeff) {
    if (imgeff == nullptr) return false;
    const auto policy = imgeff->SourceContributionPolicy();
    return policy == SceneImageEffectLayer::SourcePolicy::OwnerNodeAndProxyChildren ||
        policy == SceneImageEffectLayer::SourcePolicy::ProxyChildrenOnly;
}

static bool EffectSourceUsesOwnerNode(SceneImageEffectLayer* imgeff) {
    if (imgeff == nullptr) return true;
    const auto policy = imgeff->SourceContributionPolicy();
    return policy == SceneImageEffectLayer::SourcePolicy::OwnerNode ||
        policy == SceneImageEffectLayer::SourcePolicy::OwnerNodeAndProxyChildren;
}

static bool HasRenderOrderProxyChildren(SceneNode* node, const ExtraInfo& extra) {
    if (node == nullptr || extra.scene == nullptr) return false;
    const auto proxy_it = extra.scene->renderOrderProxyChildren.find(node);
    return proxy_it != extra.scene->renderOrderProxyChildren.end() && !proxy_it->second.empty();
}

static bool ShouldSeedEmptyProxyComposeFromFramebuffer(SceneNode* node,
                                                       SceneImageEffectLayer* imgeff,
                                                       i32 imgId,
                                                       const ExtraInfo& extra,
                                                       std::string_view inherited_output,
                                                       bool compose_source_route) {
    if (node == nullptr || imgeff == nullptr) return false;
    if (compose_source_route || inherited_output != SpecTex_Default) return false;
    if (!node->Visible()) return false;
    if (IsOffscreenDependencyLayer(extra, imgId)) return false;
    if (imgeff->SourceContributionPolicy() !=
        SceneImageEffectLayer::SourcePolicy::ProxyChildrenOnly) {
        return false;
    }
    return !HasRenderOrderProxyChildren(node, extra);
}

enum class OwnerNodeSourceFallbackReason
{
    None,
    EmptyProxyDependencySource,
};

static OwnerNodeSourceFallbackReason ResolveOwnerNodeSourceFallbackReason(
    SceneNode* node, SceneImageEffectLayer* imgeff, i32 imgId, const ExtraInfo& extra) {
    if (node == nullptr || imgeff == nullptr) return OwnerNodeSourceFallbackReason::None;
    if (imgeff->SourceContributionPolicy() !=
        SceneImageEffectLayer::SourcePolicy::ProxyChildrenOnly) {
        return OwnerNodeSourceFallbackReason::None;
    }
    if (HasRenderOrderProxyChildren(node, extra)) return OwnerNodeSourceFallbackReason::None;
    if (IsOffscreenDependencyLayer(extra, imgId)) {
        return OwnerNodeSourceFallbackReason::EmptyProxyDependencySource;
    }
    return OwnerNodeSourceFallbackReason::None;
}

static std::string_view OwnerNodeSourceFallbackReasonName(
    OwnerNodeSourceFallbackReason reason) {
    switch (reason) {
    case OwnerNodeSourceFallbackReason::None: return "none";
    case OwnerNodeSourceFallbackReason::EmptyProxyDependencySource:
        return "empty-proxy-dependency-source";
    }
    return "unknown";
}

static bool OwnerNodeSourceFallbackSamplesFramebuffer(
    SceneNode* node, OwnerNodeSourceFallbackReason reason) {
    if (reason != OwnerNodeSourceFallbackReason::EmptyProxyDependencySource) return false;
    if (node == nullptr || node->Mesh() == nullptr || node->Mesh()->Material() == nullptr) {
        return false;
    }

    const auto& textures = node->Mesh()->Material()->textures;
    return std::find(textures.begin(), textures.end(), std::string(SpecTex_Default)) !=
        textures.end();
}

struct EffectSourceRoutingDecision {
    OwnerNodeSourceFallbackReason owner_node_fallback_reason {
        OwnerNodeSourceFallbackReason::None
    };
    bool        owner_node_source_fallback { false };
    bool        owner_node_source_fallback_samples_framebuffer { false };
    bool        owner_node_contributes_to_effect_source { false };
    bool        proxy_children_contribute_to_effect_source { false };
    std::string active_compose_source_camera;
    bool        use_compose_camera_override { false };
    bool        seed_empty_proxy_compose_from_framebuffer { false };
};

static EffectSourceRoutingDecision ResolveEffectSourceRouting(SceneNode* node,
                                                              SceneImageEffectLayer* imgeff,
                                                              i32 imgId,
                                                              const ExtraInfo& extra,
                                                              std::string_view inherited_output,
                                                              const TraversalRoute& route) {
    EffectSourceRoutingDecision decision;

    // Compose source routing is the policy boundary between traversal and pass emission. Keeping the
    // derived booleans together makes each Wallpaper Engine source contract explicit: owner-card
    // contribution, proxy-child contribution, empty dependency fallback, and framebuffer seeding are
    // separate decisions even though the render graph eventually emits them in one traversal pass.
    decision.owner_node_fallback_reason =
        ResolveOwnerNodeSourceFallbackReason(node, imgeff, imgId, extra);
    decision.owner_node_source_fallback =
        decision.owner_node_fallback_reason != OwnerNodeSourceFallbackReason::None;
    decision.owner_node_source_fallback_samples_framebuffer =
        OwnerNodeSourceFallbackSamplesFramebuffer(node, decision.owner_node_fallback_reason);
    decision.owner_node_contributes_to_effect_source =
        EffectSourceUsesOwnerNode(imgeff) || decision.owner_node_source_fallback;
    decision.proxy_children_contribute_to_effect_source = EffectSourceUsesProxyChildren(imgeff);
    decision.active_compose_source_camera =
        route.compose_source
            ? route.compose_source_camera
            : (decision.proxy_children_contribute_to_effect_source && node != nullptr
                   ? std::string(node->Camera())
                   : std::string());
    decision.use_compose_camera_override =
        route.compose_source && imgeff == nullptr && !decision.active_compose_source_camera.empty();
    decision.seed_empty_proxy_compose_from_framebuffer =
        ShouldSeedEmptyProxyComposeFromFramebuffer(node,
                                                  imgeff,
                                                  imgId,
                                                  extra,
                                                  inherited_output,
                                                  route.compose_source);
    return decision;
}

static NodePassOptions BuildOwnerSourcePassOptions(
    SceneImageEffectLayer* imgeff,
    std::string_view output,
    std::string_view inherited_output,
    const TraversalRoute& route,
    const EffectSourceRoutingDecision& source_route) {
    const bool clear_private_effect_source =
        route.compose_source && imgeff != nullptr && output != inherited_output;
    const bool clear_solid_layer_effect_source =
        // Framebuffer-fed copybackground helper seeds need one clean private source target before
        // their owner draw. Clearing only this first pass keeps the generated spectrum chain from
        // inheriting stale alpha outside the authored bar mask without broadening the behavior to
        // unrelated solidlayer effects.
        !route.compose_source && imgeff != nullptr && output != inherited_output &&
        imgeff->ClearSourceBeforeOwnerDraw();

    // Owner-source emission is the one place where source routing affects actual pass state. Keep
    // these side effects grouped so future route types can extend the pass contract without adding
    // another cluster of loosely related booleans inside ToGraphPass().
    //
    // - Composition source routes write child layers into a parent-local source target, so they need
    //   alpha writes and sometimes a source-camera projection override.
    // - Empty dependency compose layers sample the live framebuffer through their owner material, so
    //   their uniforms must be evaluated against the active camera while the pass still writes its
    //   private offscreen target.
    // - Private effect source targets are cleared only at the seed step; clearing later authored
    //   effect passes would erase intermediate waterwaves/foliagesway/opacity results.
    return NodePassOptions {
        .force_alpha_write = route.compose_source,
        .clear_before_draw = clear_private_effect_source || clear_solid_layer_effect_source,
        .camera_override = source_route.use_compose_camera_override
            ? source_route.active_compose_source_camera
            : std::string(),
        .use_active_camera_for_parallax = source_route.use_compose_camera_override,
        .premultiplied_source_blend = route.premultiplied_source_blend,
        .use_active_camera_for_uniforms =
            source_route.owner_node_source_fallback_samples_framebuffer,
    };
}

static std::string_view EffectSourcePolicyName(SceneImageEffectLayer::SourcePolicy policy) {
    switch (policy) {
    case SceneImageEffectLayer::SourcePolicy::OwnerNode: return "owner-node";
    case SceneImageEffectLayer::SourcePolicy::OwnerNodeAndProxyChildren:
        return "owner-node-and-proxy-children";
    case SceneImageEffectLayer::SourcePolicy::ProxyChildrenOnly: return "proxy-children-only";
    }
    return "unknown";
}

static Eigen::Matrix4d ResolveRouteModel(SceneNode* node,
                                         const std::optional<Eigen::Matrix4d>& route_model) {
    if (route_model.has_value()) return *route_model;
    if (node == nullptr) return Eigen::Matrix4d::Identity();

    node->UpdateTrans();
    return node->ModelTrans();
}

static std::string DebugRouteMatrixSummary(const Eigen::Matrix4d& matrix) {
    char buffer[256];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "tx=%.3f ty=%.3f sx=%.3f sy=%.3f",
                  matrix(0, 3),
                  matrix(1, 3),
                  Eigen::Vector3d(matrix(0, 0), matrix(1, 0), matrix(2, 0)).norm(),
                  Eigen::Vector3d(matrix(0, 1), matrix(1, 1), matrix(2, 1)).norm());
    return std::string(buffer);
}

static std::optional<Eigen::Matrix4d> BuildChildRouteModel(
    SceneNode* parent, SceneNode* child, bool routed_child,
    const std::optional<Eigen::Matrix4d>& parent_route_model) {
    if (parent == nullptr || child == nullptr) return std::nullopt;
    if (!routed_child && !parent_route_model.has_value()) return std::nullopt;

    // Proxy routing is order-only in the physical SceneNode tree, but render-time transforms still
    // need to follow Wallpaper Engine's authored parent. Propagating a resolved route matrix lets
    // effect final passes use the same virtual parent chain that shader uniforms use later.
    const auto parent_route =
        RemoveImageAlignmentOffsetFromModel(ResolveRouteModel(parent, parent_route_model),
                                            parent->AlignmentOffset());
    return parent_route * child->GetLocalTrans();
}

static std::vector<OrderedRenderGraphChild> OrderedRenderGraphChildren(SceneNode* node,
                                                                       ExtraInfo& extra) {
    std::vector<OrderedRenderGraphChild> children;
    if (node == nullptr || extra.scene == nullptr) return children;

    std::unordered_set<SceneNode*> seen;
    size_t sequence = 0;
    for (auto& child : node->GetChildren()) {
        if (!child || !seen.insert(child.get()).second) continue;
        children.push_back(OrderedRenderGraphChild {
            .node = child.get(),
            .proxy = false,
            .sequence = sequence++,
        });
    }

    if (auto proxy_it = extra.scene->renderOrderProxyChildren.find(node);
        proxy_it != extra.scene->renderOrderProxyChildren.end()) {
        for (auto* proxy_child : proxy_it->second) {
            if (proxy_child == nullptr || !seen.insert(proxy_child).second) continue;
            children.push_back(OrderedRenderGraphChild {
                .node = proxy_child,
                .proxy = true,
                .sequence = sequence++,
            });
        }
    }

    // The scene tree still owns lifetime and transforms, but the render graph needs Wallpaper
    // Engine's authored layer order. Sorting only by known layer order and then by insertion
    // sequence keeps helper nodes deterministic without forcing every runtime node to have an
    // authored layer id.
    std::stable_sort(children.begin(), children.end(), [&extra](const auto& lhs, const auto& rhs) {
        const auto lhs_index = NodeLayerOrderIndex(lhs.node, extra);
        const auto rhs_index = NodeLayerOrderIndex(rhs.node, extra);
        if (lhs_index != rhs_index) return lhs_index < rhs_index;
        return lhs.sequence < rhs.sequence;
    });
    return children;
}

static void AddNodePass(SceneNode* node, std::string_view output, i32 imgId, ExtraInfo& extra,
                        std::function<bool()> should_execute = {},
                        NodePassOptions options = {}) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    if (node->Mesh() == nullptr) {
        return;
    }
    auto* mesh = node->Mesh();
    if (mesh->Material() == nullptr) {
        return;
    }
    auto* material = mesh->Material();
    const std::string output_key =
        material->modelRenderState.has_value() && !material->modelRenderState->outputOverride.empty()
            ? material->modelRenderState->outputOverride
            : std::string(output);
    const bool is_model_pass = material->modelRenderState.has_value();
    const bool clear_model_depth = is_model_pass &&
        extra.model_depth_outputs_seen.insert(output_key).second;
    if (WallpaperDebugLayerEnabled(imgId)) {
        LOG_INFO("DebugLayerGraphNodePass: layer=%d node='%s' material='%s' output='%s' "
                 "textures=%zu clear-before=%s force-alpha=%s premultiplied-source=%s "
                 "camera-override='%s' active-camera-uniforms=%s model-pass=%s",
                 imgId,
                 node != nullptr ? node->Name().c_str() : "",
                 material != nullptr ? material->name.c_str() : "",
                 output_key.c_str(),
                 material != nullptr ? material->textures.size() : 0,
                 options.clear_before_draw ? "true" : "false",
                 options.force_alpha_write ? "true" : "false",
                 options.premultiplied_source_blend ? "true" : "false",
                 options.camera_override.c_str(),
                 options.use_active_camera_for_uniforms ? "true" : "false",
                 is_model_pass ? "true" : "false");
    }

    std::string passName = material->name;
    rgraph.addPass<vulkan::CustomShaderPass>(
        passName,
        rg::PassNode::Type::CustomShader,
        [material, node, output_key, imgId, &rgraph, &scene, &extra,
         clear_model_depth, options = std::move(options),
         should_execute = std::move(should_execute)](
            rg::RenderGraphBuilder& builder, vulkan::CustomShaderPass::Desc& pdesc) {
            const auto& pass = builder.workPassNode();
            // Passing the live scene into the prepared pass lets resource refreshes resolve current
            // render-target dependencies directly, which is what keeps first-class text bridges and
            // ordinary effect passes on the same stable render-graph contract.
            pdesc.scene      = &scene;
            pdesc.node       = node;
            pdesc.layer_id   = imgId;
            pdesc.execute_when_hidden = ShouldExecuteHiddenDependency(scene, node, output_key);
            pdesc.should_execute      = should_execute;
            pdesc.output     = output_key;
            pdesc.force_alpha_write = output_key != SpecTex_Default && options.force_alpha_write;
            pdesc.premultiplied_source_blend = options.premultiplied_source_blend;
            pdesc.clear_before_draw = output_key != SpecTex_Default && options.clear_before_draw;
            pdesc.camera_override = options.camera_override;
            pdesc.use_active_camera_for_uniforms = options.use_active_camera_for_uniforms;
            pdesc.use_active_camera_for_parallax =
                !pdesc.camera_override.empty() && options.use_active_camera_for_parallax;
            if (!pdesc.camera_override.empty()) {
                LOG_VERBOSE("SceneRenderGraphComposeCameraOverride: layer=%d node='%s' "
                         "output='%s' camera='%s' active-parallax=%s",
                         imgId,
                         node != nullptr ? node->Name().c_str() : "",
                         output_key.c_str(),
                         pdesc.camera_override.c_str(),
                         pdesc.use_active_camera_for_parallax ? "true" : "false");
            }
            if (pdesc.use_active_camera_for_uniforms) {
                LOG_VERBOSE("SceneRenderGraphActiveCameraUniformOverride: layer=%d node='%s' "
                         "output='%s'",
                         imgId,
                         node != nullptr ? node->Name().c_str() : "",
                         output_key.c_str());
            }
            if (const auto& model_state = material->modelRenderState; model_state.has_value()) {
                // Depth state is transported through the pass description instead of inferred from
                // camera names, so adding model rendering cannot alter ordinary 2D custom shaders.
                pdesc.model_pass = true;
                pdesc.depth_test = model_state->depthTest;
                pdesc.depth_write = model_state->depthWrite;
                pdesc.clear_depth = clear_model_depth;
            }
            CheckAndSetSprite(scene, pdesc, material->textures);
            for (usize i = 0; i < material->textures.size(); i++) {
                const auto&  url = material->textures[i];
                rg::TexNode* input { nullptr };
                if (url.empty()) {
                    pdesc.textures.emplace_back("");
                    continue;
                } else if (IsSpecLinkTex(url)) {
                    auto id = ParseLinkTex(url);
                    extra.link_info.push_back(
                        DelayLinkInfo { .id = pass.ID(), .link_id = id, .tex_index = (i32)i });
                    pdesc.textures.emplace_back("");
                    continue;
                } else {
                    rg::TexNode::Desc desc;
                    desc.key  = url;
                    desc.name = url;
                    // Some effect-local FBOs use plain names such as `blur_start_2_<addr>`.
                    // Treat any key already registered in Scene::renderTargets as temporary graph
                    // storage so those edges order like internal render targets, not external
                    // material textures.
                    desc.type = ! rg::IsRuntimeRenderTarget(&scene, url)
                        ? rg::TexNode::TexType::Imported
                        : rg::TexNode::TexType::Temp;
                    if (sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER)) {
                        input = AddMipFramebufferCopy(rgraph, builder);
                    } else {
                        input = builder.createTexNode(desc);
                    }
                    if (rg::IsRuntimeRenderTarget(&scene, url) &&
                        ! sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER)) {
                        builder.markVirtualWrite(input);
                    }
                }

                if (url == output_key) {
                    builder.markSelfWrite(input);
                    input = rg::addCopyPass(rgraph, input);
                }
                builder.read(input);
                pdesc.textures.emplace_back(input->key());
            }
            if (WallpaperDebugLayerEnabled(imgId)) {
                std::ostringstream textures_oss;
                textures_oss << "[";
                for (size_t idx = 0; idx < pdesc.textures.size(); idx++) {
                    if (idx != 0) textures_oss << ", ";
                    textures_oss << pdesc.textures[idx];
                }
                textures_oss << "]";
                LOG_INFO("DebugLayerGraphNodePassPrepared: layer=%d node='%s' material='%s' "
                         "output='%s' execute-when-hidden=%s textures=%s",
                         imgId,
                         node != nullptr ? node->Name().c_str() : "",
                         material != nullptr ? material->name.c_str() : "",
                         output_key.c_str(),
                         pdesc.execute_when_hidden ? "true" : "false",
                         textures_oss.str().c_str());
            }

            rg::TexNode* output_node { nullptr };
            output_node =
                builder.createTexNode(rg::TexNode::Desc { .name = output_key,
                                                          .key  = output_key,
                                                          .type = rg::TexNode::TexType::Temp },
                                      true);
            builder.write(output_node);
            if (ShouldPublishLayerLinkOutput(extra, imgId, output_key)) {
                extra.id_link_map[(usize)imgId] = output_node;
            }
        });
}

static void AddTextNodePass(SceneNode* node, std::string_view output, i32 imgId, ExtraInfo& extra) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    if (node == nullptr || node->Text() == nullptr) {
        return;
    }

    const std::string output_key(output);
    std::string pass_name = node->Name().empty() ? std::string("text") : node->Name();
    rgraph.addPass<vulkan::TextPass>(
        pass_name,
        rg::PassNode::Type::Text,
        [node, output_key, imgId, &scene, &extra](
            rg::RenderGraphBuilder& builder, vulkan::TextPass::Desc& pdesc) {
            const auto& pass = builder.workPassNode();
            // Text is now emitted as its own render-graph pass. It shares the same constrained
            // hidden-dependency rule as mesh passes: invisible helper layers may render private
            // offscreen sources, but they must not composite text directly into `_rt_default`.
            pdesc.scene = &scene;
            pdesc.node = node;
            // Keep the authored layer id on the prepared pass so runtime text rerasters can
            // refresh the exact Clock/TextPass resources without broadening the dirty target set.
            pdesc.layer_id = imgId;
            pdesc.execute_when_hidden = ShouldExecuteHiddenDependency(scene, node, output_key);
            pdesc.output = output_key;

            auto* output_node =
                builder.createTexNode(rg::TexNode::Desc { .name = output_key,
                                                          .key = output_key,
                                                          .type = rg::TexNode::TexType::Temp },
                                      true);
            builder.write(output_node);
            if (ShouldPublishLayerLinkOutput(extra, imgId, output_key)) {
                extra.id_link_map[(usize)imgId] = output_node;
            }
            (void)pass;
        });
}

static void ToGraphPass(SceneNode* node, std::string_view inherited_output, i32 imgId,
                        ExtraInfo& extra, std::function<bool()> node_execute_gate = {},
                        TraversalRoute route = {}) {
    auto& scene = *extra.scene;

    if (node != nullptr && !route.routed_node) {
        const bool proxy_node = scene.renderOrderProxyNodes.count(node) != 0;
        const bool detached_source_node = scene.detachedEffectSourceNodes.count(node) != 0;
        if (proxy_node || detached_source_node) {
            // Root-owned proxy/source nodes are emitted through explicit authored-order routes.
            // Reaching them through the physical tree means the root traversal is at the wrong
            // sibling position, so skip this visit to avoid late duplicate composites.
            LOG_VERBOSE("SceneRenderGraphNodeRouteSkip: layer=%d name='%s' reason='%s'",
                     NodeLayerId(scene, node),
                     node->Name().c_str(),
                     detached_source_node ? "detached-source" : "proxy");
            return;
        }
    }

    if (node != nullptr && !ShouldEmitLayerNodeForResidency(scene, node, extra)) {
        LOG_VERBOSE("SceneRenderGraphResidencySkip: layer=%d name='%s' local-visible=%s "
                 "layer-visible=%s",
                 NodeLayerId(scene, node),
                 node->Name().c_str(),
                 scene.GetLayerLocalVisibility(NodeLayerId(scene, node)) ? "true" : "false",
                 node->LayerVisible() ? "true" : "false");
        return;
    }

    std::string_view         output = inherited_output;
    SceneImageEffectLayer*   imgeff { nullptr };
    const auto resolved_route_model = ResolveRouteModel(node, route.model);
    if (node != nullptr && !node->Camera().empty()) {
        auto camera_it = scene.cameras.find(node->Camera());
        if (camera_it != scene.cameras.end() && camera_it->second->HasImgEffect()) {
            imgeff = camera_it->second->GetImgEffect().get();
            output = imgeff->FirstTarget();
        }
    }

    const auto source_route =
        ResolveEffectSourceRouting(node, imgeff, imgId, extra, inherited_output, route);
    if (imgeff != nullptr &&
        imgeff->SourceContributionPolicy() == SceneImageEffectLayer::SourcePolicy::ProxyChildrenOnly) {
        LOG_VERBOSE("SceneRenderGraphComposeSourceClear: layer=%d name='%s' output='%.*s' "
                 "camera='%s'",
                 NodeLayerId(scene, node),
                 node != nullptr ? node->Name().c_str() : "",
                 static_cast<int>(output.size()),
                 output.data(),
                 source_route.active_compose_source_camera.c_str());
        rg::addClearPass(*extra.rgraph, rg::createTexDesc(std::string(output), extra.scene));
    }

    if (source_route.seed_empty_proxy_compose_from_framebuffer &&
        HasRenderableMeshMaterial(node)) {
        LOG_VERBOSE("SceneRenderGraphComposeFramebufferSeed: layer=%d name='%s' output='%.*s' "
                 "policy=%.*s reason='empty-proxy-visible-framebuffer-source'",
                 NodeLayerId(scene, node),
                 node->Name().c_str(),
                 static_cast<int>(output.size()),
                 output.data(),
                 static_cast<int>(EffectSourcePolicyName(imgeff->SourceContributionPolicy()).size()),
                 EffectSourcePolicyName(imgeff->SourceContributionPolicy()).data());
        // Empty visible compose layers are framebuffer filters: their first effect texture is the
        // current screen image, not the composelayer card itself. The composelayer shader writes a
        // full offscreen source target while sampling `_rt_default` through the active camera, so
        // the later world-space effect writer replaces exactly the same screen region without
        // exposing a rectangular owner mesh.
        AddNodePass(node,
                    output,
                    imgId,
                    extra,
                    node_execute_gate,
                    NodePassOptions { .use_active_camera_for_uniforms = true });
    }

    if (HasRenderableMeshMaterial(node) &&
        source_route.owner_node_contributes_to_effect_source) {
        if (source_route.owner_node_source_fallback) {
            LOG_VERBOSE("SceneRenderGraphComposeOwnerSourceFallback: layer=%d name='%s' output='%.*s' "
                     "policy=%.*s reason='%.*s' framebuffer-uniforms=%s",
                     NodeLayerId(scene, node),
                     node->Name().c_str(),
                     static_cast<int>(output.size()),
                     output.data(),
                     static_cast<int>(EffectSourcePolicyName(imgeff->SourceContributionPolicy()).size()),
                     EffectSourcePolicyName(imgeff->SourceContributionPolicy()).data(),
                     static_cast<int>(
                         OwnerNodeSourceFallbackReasonName(
                             source_route.owner_node_fallback_reason)
                             .size()),
                     OwnerNodeSourceFallbackReasonName(source_route.owner_node_fallback_reason)
                         .data(),
                     source_route.owner_node_source_fallback_samples_framebuffer ? "true"
                                                                                 : "false");
        }
        AddNodePass(node,
                    output,
                    imgId,
                    extra,
                    node_execute_gate,
                    BuildOwnerSourcePassOptions(
                        imgeff, output, inherited_output, route, source_route));
    } else if (HasRenderableMeshMaterial(node) && imgeff != nullptr &&
               !source_route.seed_empty_proxy_compose_from_framebuffer) {
        LOG_VERBOSE("SceneRenderGraphComposeOwnerSourceSkip: layer=%d name='%s' output='%.*s' "
                 "policy=%.*s",
                 NodeLayerId(scene, node),
                 node->Name().c_str(),
                 static_cast<int>(output.size()),
                 output.data(),
                 static_cast<int>(EffectSourcePolicyName(imgeff->SourceContributionPolicy()).size()),
                 EffectSourcePolicyName(imgeff->SourceContributionPolicy()).data());
    }
    // Text is now a first-class scene primitive. Whenever a node owns text we emit the dedicated
    // text pass directly from that primitive, keeping the render graph aligned with the same
    // authoritative text object that parser and runtime updates mutate.
    if (node != nullptr && node->Text() != nullptr) {
        AddTextNodePass(node, output, imgId, extra);
    }

    if (node != nullptr) {
        if (auto detached_it = scene.detachedEffectSourceNodesByWorldNode.find(node);
            detached_it != scene.detachedEffectSourceNodesByWorldNode.end()) {
            for (auto* source_node : detached_it->second) {
                if (source_node == nullptr) continue;
                LOG_VERBOSE("SceneRenderGraphDetachedSourceRoute: world-layer=%d source-layer=%d "
                         "world-name='%s' source-name='%s' output='%.*s'",
                         NodeLayerId(scene, node),
                         NodeLayerId(scene, source_node),
                         node->Name().c_str(),
                         source_node->Name().c_str(),
                         static_cast<int>(output.size()),
                         output.data());
                ToGraphPass(source_node,
                            output,
                            NodeLayerId(scene, source_node),
                            extra,
                            {},
                            TraversalRoute {
                                .routed_node = true,
                                // Detached source nodes render through their own effect camera, but
                                // the image-effect final writer belongs to the visible world node.
                                // Forward the world route matrix so the final pass inherits virtual
                                // render-order parents while intermediate effect passes stay local.
                                .model = std::optional<Eigen::Matrix4d> { resolved_route_model },
                                .compose_source = route.compose_source,
                                .compose_source_camera =
                                    source_route.active_compose_source_camera });
            }
        }
    }

    std::vector<OrderedRenderGraphChild> deferred_proxy_children;
    if (node != nullptr) {
        for (const auto& child : OrderedRenderGraphChildren(node, extra)) {
            if (child.node == nullptr) continue;
            if (child.proxy && imgeff != nullptr &&
                !source_route.proxy_children_contribute_to_effect_source &&
                !IsEffectLocalProxyDependency(child.node, extra)) {
                // A render-order proxy edge only restores authored sibling order. It does not make
                // the proxied world-space node a real child of this image-effect source target, so
                // defer ordinary proxies until the parent effect has resolved back to the inherited
                // output space.
                LOG_VERBOSE("SceneRenderGraphProxyChildDefer: parent-layer=%d proxy-layer=%d "
                         "inherited-output='%.*s' parent-effect-output='%.*s'",
                         NodeLayerId(scene, node),
                         NodeLayerId(scene, child.node),
                         static_cast<int>(inherited_output.size()),
                         inherited_output.data(),
                         static_cast<int>(output.size()),
                         output.data());
                deferred_proxy_children.push_back(child);
                continue;
            }

            if (child.proxy) {
                if (imgeff != nullptr) {
                    if (source_route.proxy_children_contribute_to_effect_source &&
                        !IsEffectLocalProxyDependency(child.node, extra)) {
                        LOG_VERBOSE("SceneRenderGraphProxyComposeSourceRoute: parent-layer=%d "
                                 "proxy-layer=%d output='%.*s' policy=%.*s camera='%s'",
                                 NodeLayerId(scene, node),
                                 NodeLayerId(scene, child.node),
                                 static_cast<int>(output.size()),
                                 output.data(),
                                 static_cast<int>(
                                     EffectSourcePolicyName(imgeff->SourceContributionPolicy()).size()),
                                 EffectSourcePolicyName(imgeff->SourceContributionPolicy()).data(),
                                 source_route.active_compose_source_camera.c_str());
                    } else {
                        // Wallpaper Engine `dependencies` are effect-local inputs. These proxies
                        // must stay inside the parent effect phase because the parent shader samples
                        // their private source target while resolving the effect chain.
                        LOG_VERBOSE("SceneRenderGraphProxyInlineEffectRoute: parent-layer=%d "
                                 "proxy-layer=%d output='%.*s'",
                                 NodeLayerId(scene, node),
                                 NodeLayerId(scene, child.node),
                                 static_cast<int>(output.size()),
                                 output.data());
                    }
                } else {
                    LOG_VERBOSE("SceneRenderGraphProxyChildRoute: parent-layer=%d proxy-layer=%d "
                             "name='%s' output='%.*s'",
                             NodeLayerId(scene, node),
                             NodeLayerId(scene, child.node),
                             child.node->Name().c_str(),
                             static_cast<int>(output.size()),
                             output.data());
                }
            }
            const bool child_compose_source_route =
                route.compose_source ||
                (child.proxy && imgeff != nullptr &&
                 source_route.proxy_children_contribute_to_effect_source &&
                 !IsEffectLocalProxyDependency(child.node, extra));
            const std::string child_compose_source_camera =
                child_compose_source_route ? source_route.active_compose_source_camera
                                           : std::string();
            ToGraphPass(child.node,
                        output,
                        NodeLayerId(scene, child.node),
                        extra,
                        {},
                        TraversalRoute {
                            .routed_node = child.proxy,
                            .model = BuildChildRouteModel(node, child.node, child.proxy, route.model),
                            .compose_source = child_compose_source_route,
                            .compose_source_camera = child_compose_source_camera });
        }
    }

    if (imgeff != nullptr) {
        // Composite source nodes may now be transform-only containers whose children draw into the
        // effect source target. Resolving the effect after all descendants have emitted their passes
        // keeps those composite layers correct without requiring each child to manage effect timing.
        // Hidden dependency layers are not visible screen compositors. When a later effect samples
        // `_rt_imageLayerComposite_<id>`, the dependency's final authored effect must still resolve
        // into its private ping-pong target; otherwise the generic visible-layer rewrite would move
        // that final pass to `_rt_default`, where the hidden-layer execution gate correctly skips it.
        const auto resolved_effect_world_affine =
            Eigen::Affine3f(resolved_route_model.cast<float>());
        const bool keep_final_output_private =
            ShouldKeepEffectFinalOutputPrivate(extra, node, imgId, inherited_output);
        const auto final_output_policy =
            route.compose_source
                ? SceneImageEffectLayer::FinalOutputPolicy::PrivateAuthoredThenComposite
                : SceneImageEffectLayer::FinalOutputPolicy::AuthoredWriter;
        LOG_VERBOSE("SceneRenderGraphEffectResolve: layer=%d name='%s' inherited-output='%.*s' "
                 "effect-output='%.*s' offscreen-dependency=%s visible=%s local-visible=%s "
                 "routed=%s keep-final-private=%s compose-source-route=%s compose-camera='%s'",
                 NodeLayerId(scene, node),
                 node != nullptr ? node->Name().c_str() : "",
                 static_cast<int>(inherited_output.size()),
                 inherited_output.data(),
                 static_cast<int>(output.size()),
                 output.data(),
                 IsOffscreenDependencyLayer(extra, imgId) ? "true" : "false",
                 node != nullptr && node->Visible() ? "true" : "false",
                 node != nullptr && node->LocalVisible() ? "true" : "false",
                 route.routed_node ? "true" : "false",
                 keep_final_output_private ? "true" : "false",
                 route.compose_source ? "true" : "false",
                 source_route.active_compose_source_camera.c_str());
        if (WallpaperDebugLayerEnabled(imgId)) {
            LOG_INFO("DebugLayerEffectRouteModel: layer=%d name='%s' route=%s compose-source=%s",
                     imgId,
                     node != nullptr ? node->Name().c_str() : "",
                     DebugRouteMatrixSummary(resolved_route_model).c_str(),
                     route.compose_source ? "true" : "false");
        }
        imgeff->ResolveEffect(scene.default_effect_mesh,
                              "effect",
                              node != nullptr ? std::string_view(node->Camera()) : std::string_view(),
                              inherited_output,
                              keep_final_output_private,
                              &resolved_effect_world_affine,
                              final_output_policy);

        for (usize i = 0; i < imgeff->EffectCount(); i++) {
            auto& eff     = imgeff->GetEffect(i);
            auto  cmdItor = eff->commands.begin();
            auto  cmdEnd  = eff->commands.end();
            int   nodePos = 0;
            auto  effect_visible_gate = [eff]() {
                return eff == nullptr || eff->LocalVisible();
            };
            auto effect_hidden_gate = [eff]() {
                return eff != nullptr && !eff->LocalVisible();
            };
            for (auto& effect_node : eff->nodes) {
                if (cmdItor != cmdEnd && nodePos == cmdItor->afterpos) {
                    rg::addCopyPass(*extra.rgraph,
                                    rg::createTexDesc(cmdItor->src, extra.scene),
                                    rg::createTexDesc(cmdItor->dst, extra.scene),
                                    effect_visible_gate);
                    cmdItor++;
                }
                // Effect material nodes are private render-graph passes. They can carry a camera
                // override for layer-surface publication, but they must not be traversed through
                // ToGraphPass(): an override camera may itself own the same SceneImageEffectLayer,
                // and treating this internal pass as another image-effect owner recursively
                // rebuilds the chain while it is being emitted, overwriting the already-resolved
                // puppet and layer-surface publication route.
                AddNodePass(effect_node.sceneNode.get(),
                            effect_node.output,
                            imgId,
                            extra,
                            effect_visible_gate,
                            NodePassOptions {
                                .force_alpha_write = effect_node.force_alpha_write,
                                .clear_before_draw = effect_node.clear_before_draw,
                                .camera_override = effect_node.camera_override,
                                .use_active_camera_for_parallax =
                                    effect_node.use_active_camera_for_parallax });
                nodePos++;
            }
            if (!eff->BypassSource().empty() && !eff->BypassTarget().empty() &&
                eff->BypassSource() != eff->BypassTarget()) {
                // Hidden effects must still advance the ping-pong chain. The shader and authored
                // effect commands become no-ops through their local visibility, then this gated copy
                // forwards the current input target to the effect output target so downstream
                // effects sample the current frame, not the stale texture from the last visible
                // frame.
                rg::addCopyPass(*extra.rgraph,
                                rg::createTexDesc(eff->BypassSource(), extra.scene),
                                rg::createTexDesc(eff->BypassTarget(), extra.scene),
                                effect_hidden_gate);
            }
        }

        if (imgeff->HasFinalComposite()) {
            // The image-effect layer decides whether the neutral final composite is acting as the
            // ordinary stable publisher or only as a hidden-effect bypass fallback. Keeping that
            // policy inside SceneImageEffectLayer avoids shader/name special cases in the graph
            // traversal while still preserving direct authored final writers for static chains.
            const bool should_run_final_composite =
                imgeff != nullptr && imgeff->ShouldRunFinalComposite();
            imgeff->FinalNode().SetLocalVisible(should_run_final_composite);
            auto final_composite_gate = [imgeff]() {
                return imgeff != nullptr && imgeff->ShouldRunFinalComposite();
            };
            ToGraphPass(&imgeff->FinalNode(),
                        inherited_output,
                        imgId,
                        extra,
                        final_composite_gate,
                        TraversalRoute {
                            // The synthetic fallback is another detached final writer for the same
                            // image effect, so it must share the resolved route matrix used by the
                            // authored final output instead of recomputing from its physical tree.
                            .model = std::optional<Eigen::Matrix4d> { resolved_route_model },
                            .compose_source = route.compose_source,
                            .compose_source_camera = source_route.active_compose_source_camera,
                            .premultiplied_source_blend =
                                imgeff->FinalCompositeSamplesPremultipliedSource() });
        }
    }

    for (const auto& child : deferred_proxy_children) {
        if (child.node == nullptr) continue;
        LOG_VERBOSE("SceneRenderGraphProxyOutputRoute: parent-layer=%d proxy-layer=%d output='%.*s'",
                 NodeLayerId(scene, node),
                 NodeLayerId(scene, child.node),
                 static_cast<int>(inherited_output.size()),
                 inherited_output.data());
        ToGraphPass(child.node,
                    inherited_output,
                    NodeLayerId(scene, child.node),
                    extra,
                    {},
                    TraversalRoute {
                        .routed_node = true,
                        .model = BuildChildRouteModel(node, child.node, true, route.model),
                        .compose_source = route.compose_source,
                        .compose_source_camera = source_route.active_compose_source_camera });
    }
}

static std::unique_ptr<rg::RenderGraph> SceneToRenderGraphImpl(
    Scene& scene, bool include_hidden_for_pipeline_warmup) {
    std::unique_ptr<rg::RenderGraph> rgraph = std::make_unique<rg::RenderGraph>();
    ExtraInfo                        extra { .rgraph = rgraph.get(),
                                             .scene = &scene,
                                             .include_hidden_for_pipeline_warmup =
                                                 include_hidden_for_pipeline_warmup };
    for (size_t index = 0; index < scene.layerOrder.size(); index++) {
        extra.layer_order_index[scene.layerOrder[index]] = index;
    }
    LOG_VERBOSE("SceneRenderGraphOrderInit: layer-count=%zu proxy-parent-count=%zu proxy-node-count=%zu "
             "detached-anchor-count=%zu detached-source-count=%zu warmup-hidden=%s",
             scene.layerOrder.size(),
             scene.renderOrderProxyChildren.size(),
             scene.renderOrderProxyNodes.size(),
             scene.detachedEffectSourceNodesByWorldNode.size(),
             scene.detachedEffectSourceNodes.size(),
             include_hidden_for_pipeline_warmup ? "true" : "false");
    ToGraphPass(scene.sceneGraph.get(), SpecTex_Default, scene.sceneGraph->ID(), extra);

    for (auto& info : extra.link_info) {
        if (! exists(extra.id_link_map, info.link_id)) {
            LOG_ERROR("link tex %d not found", info.link_id);
            continue;
        }
        rgraph->afterBuild(
            info.id, [&rgraph, &extra, &info](rg::RenderGraphBuilder& builder, rg::Pass& rgpass) {
                auto& pass = static_cast<vulkan::CustomShaderPass&>(rgpass);

                auto* link_tex_node = extra.id_link_map.at(info.link_id);
                auto  copy_desc     = link_tex_node->genDesc();
                copy_desc.key       = GenLinkTex((idx)info.link_id);
                copy_desc.name      = copy_desc.key;

                auto new_in = rg::addCopyPass(*rgraph, link_tex_node, &copy_desc);
                builder.read(new_in);
                pass.setDescTex((u32)info.tex_index, new_in->key());
                return true;
            });
    }

    if (!scene.bloom.nodes.empty()) {
        // Scene Bloom is authored in `general`, so it belongs after the complete layer traversal.
        // Wallpaper Engine implements it as an ordered post-process chain. Binding each synthetic
        // node with its explicit output target preserves the quarter/eighth render-target feedback
        // contract, while the first pass still uses `u_enabled` to make runtime toggles cheap.
        if (scene.bloom.nodes.size() != scene.bloom.outputs.size()) {
            LOG_ERROR("SceneBloomGraphBind: pass/output mismatch passes=%zu outputs=%zu",
                      scene.bloom.nodes.size(),
                      scene.bloom.outputs.size());
        } else {
            LOG_VERBOSE("SceneBloomGraphBind: passes=%zu enabled=%s strength=%.3f threshold=%.3f",
                        scene.bloom.nodes.size(),
                        scene.bloom.enabled ? "true" : "false",
                        scene.bloom.strength,
                        scene.bloom.threshold);
            for (usize i = 0; i < scene.bloom.nodes.size(); ++i) {
                if (scene.bloom.nodes[i] == nullptr) {
                    LOG_ERROR("SceneBloomGraphBind: missing pass index=%zu output='%s'",
                              i,
                              scene.bloom.outputs[i].c_str());
                    continue;
                }
                LOG_VERBOSE("SceneBloomGraphBind: pass=%zu node='%s' output='%s'",
                            i,
                            scene.bloom.nodes[i]->Name().c_str(),
                            scene.bloom.outputs[i].c_str());
                AddNodePass(scene.bloom.nodes[i].get(), scene.bloom.outputs[i], 0, extra);
            }
        }
    } else if (scene.bloom.node != nullptr) {
        // This fallback is intentionally retained for older parsed scene objects that may still
        // populate only the legacy single-node field before a full reparse has occurred.
        LOG_VERBOSE("SceneBloomGraphBind: legacy-output='%s' enabled=%s strength=%.3f threshold=%.3f",
                    SpecTex_Default.data(),
                    scene.bloom.enabled ? "true" : "false",
                    scene.bloom.strength,
                    scene.bloom.threshold);
        AddNodePass(scene.bloom.node.get(), SpecTex_Default, 0, extra);
    }

    return rgraph;
}

std::unique_ptr<rg::RenderGraph> wallpaper::sceneToRenderGraph(Scene& scene) {
    return SceneToRenderGraphImpl(scene, false);
}

std::unique_ptr<rg::RenderGraph> wallpaper::sceneToPipelineWarmupRenderGraph(Scene& scene) {
    return SceneToRenderGraphImpl(scene, true);
}
