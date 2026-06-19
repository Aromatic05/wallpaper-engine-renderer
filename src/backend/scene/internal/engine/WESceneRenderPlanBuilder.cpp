#include "WESceneRenderPlanBuilder.hpp"

#include "scene/Scene.h"
#include "rendergraph/RenderGraph.hpp"
#include "SpecTexs.hpp"
#include "utils/Logging.h"
#include "core/MapSet.hpp"

#include "render/vulkanrender/AllPasses.hpp"

#include <array>

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
    return IsSpecTex(path) || (scene != nullptr && scene->renderTargets.count(path) != 0);
}

static TexNode::Desc createTexDesc(std::string path, const Scene* scene = nullptr) {
    return TexNode::Desc { .name = path,
                           .key  = path,
                           .type = IsRuntimeRenderTarget(scene, path) ? TexNode::TexType::Temp
                                                                      : TexNode::TexType::Imported };
}
} // namespace wallpaper::rg

static void TraverseNode(const std::function<void(SceneNode*)>& func, SceneNode* node) {
    if (node == nullptr || ! node->Visible()) return;
    func(node);
    for (auto& child : node->GetChildren()) TraverseNode(func, child.get());
}

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
    bool                       use_mipmap_framebuffer { false };
};

static void ToGraphPass(SceneNode* node, std::string_view output, i32 imgId, ExtraInfo& extra,
                        const SceneImageEffectNode* effect_node = nullptr,
                        const SceneImageEffect* effect_owner = nullptr,
                        std::string_view camera_override = {}, bool clear_before_draw = false,
                        bool force_alpha_write = false,
                        bool premultiplied_source_blend = false) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    auto loadEffect = [node, &rgraph, &scene, &extra](SceneImageEffectLayer* effs) {
        effs->ResolveEffect(scene.default_effect_mesh, "effect");

        for (usize i = 0; i < effs->EffectCount(); i++) {
            auto& eff     = effs->GetEffect(i);
            auto  cmdItor = eff->commands.begin();
            auto  cmdEnd  = eff->commands.end();
            int   nodePos = 0;
            for (auto& n : eff->nodes) {
                if (cmdItor != cmdEnd && nodePos == cmdItor->afterpos) {
                    rg::addCopyPass(
                        rgraph,
                        rg::createTexDesc(cmdItor->src, &scene),
                        rg::createTexDesc(cmdItor->dst, &scene),
                        [eff = eff.get()] {
                            return eff->LocalVisible();
                        });
                    cmdItor++;
                }
                auto& name = n.output;
                ToGraphPass(n.sceneNode.get(), name, node->ID(), extra, &n, eff.get());
                nodePos++;
            }
            rg::addCopyPass(
                rgraph,
                rg::createTexDesc(eff->BypassSource(), &scene),
                rg::createTexDesc(eff->BypassTarget(), &scene),
                [eff = eff.get()] {
                    return ! eff->LocalVisible();
                });
            if (! eff->FinalBypassTarget().empty() && eff->FinalBypassTarget() != eff->BypassTarget()) {
                rg::addCopyPass(
                    rgraph,
                    rg::createTexDesc(eff->BypassSource(), &scene),
                    rg::createTexDesc(eff->FinalBypassTarget(), &scene),
                    [eff = eff.get()] {
                        return ! eff->LocalVisible();
                    });
            }
        }

        if (effs->HasFinalComposite()) {
            ToGraphPass(&effs->FinalNode(),
                        SpecTex_Default,
                        node->ID(),
                        extra,
                        nullptr,
                        nullptr,
                        effs->FinalNode().Camera(),
                        false,
                        false,
                        true);
        }
    };

    auto addTextPass = [node, output, imgId, &rgraph, &scene]() {
        if (node == nullptr || scene.textPrimitives.count(imgId) == 0 ||
            scene.textPrimitives.at(imgId) == nullptr) {
            return;
        }
        if (scene.renderTargets.count(std::string(output)) == 0) {
            LOG_ERROR("TextPass: output render target not found while building graph: %.*s",
                      static_cast<int>(output.size()),
                      output.data());
            return;
        }

        rgraph.addPass<vulkan::TextPass>(
            "text",
            rg::PassNode::Type::Text,
            [node, output = std::string(output), imgId, &scene](
                rg::RenderGraphBuilder& builder, vulkan::TextPass::Desc& pdesc) {
                pdesc.scene    = &scene;
                pdesc.node     = node;
                pdesc.layer_id = imgId;
                pdesc.output   = output;

                auto* output_node =
                    builder.createTexNode(rg::TexNode::Desc { .name = output,
                                                              .key  = output,
                                                              .type = rg::TexNode::TexType::Temp },
                                          true);
                builder.write(output_node);
            });
    };

    addTextPass();

    if (auto primitive_it = scene.textPrimitives.find(imgId);
        primitive_it != scene.textPrimitives.end() && primitive_it->second != nullptr &&
        primitive_it->second->bridge.enabled) {
        for (const auto& target : primitive_it->second->bridge.render_targets) {
            if (target.name.empty() || scene.renderTargets.count(target.name) == 0) continue;
            rg::addClearPass(rgraph, rg::createTexDesc(target.name, &scene));
            rgraph.addPass<vulkan::TextPass>(
                "text",
                rg::PassNode::Type::Text,
                [node, target_name = target.name, imgId, &scene](
                    rg::RenderGraphBuilder& builder, vulkan::TextPass::Desc& pdesc) {
                    pdesc.scene        = &scene;
                    pdesc.node         = node;
                    pdesc.layer_id     = imgId;
                    pdesc.output       = target_name;
                    pdesc.clear_output = true;

                    auto* output_node =
                        builder.createTexNode(rg::TexNode::Desc { .name = target_name,
                                                                  .key  = target_name,
                                                                  .type = rg::TexNode::TexType::Temp },
                                              true);
                    builder.write(output_node);
                });
        }
    }

    if (node->Mesh() == nullptr) return;
    auto* mesh = node->Mesh();
    if (mesh->Material() == nullptr) return;
    auto* material = mesh->Material();

    SceneImageEffectLayer* imgeff = nullptr;
    if (! node->Camera().empty()) {
        auto& cam = scene.cameras.at(node->Camera());
        if (cam->HasImgEffect()) {
            imgeff = cam->GetImgEffect().get();
            output = imgeff->FirstTarget();
        }
    }

    std::string passName = material->name;

    rgraph.addPass<vulkan::CustomShaderPass>(
        passName,
        rg::PassNode::Type::CustomShader,
        [material,
         node,
         effect_node,
         effect_owner,
         camera_override = std::string(camera_override),
         clear_before_draw,
         force_alpha_write,
         premultiplied_source_blend,
         &output,
         &imgId,
         &rgraph,
         &scene,
         &extra](
            rg::RenderGraphBuilder& builder, vulkan::CustomShaderPass::Desc& pdesc) {
            const auto& pass = builder.workPassNode();
            pdesc.node       = node;
            pdesc.output     = output;
            if (effect_node != nullptr) {
                pdesc.cameraOverride = effect_node->camera_override;
                pdesc.clearBeforeDraw = effect_node->clear_before_draw;
                pdesc.forceAlphaWrite = effect_node->force_alpha_write;
                pdesc.premultipliedSourceBlend = effect_node->premultiplied_source_blend;
                pdesc.should_execute = [effect_owner] {
                    return effect_owner == nullptr || effect_owner->LocalVisible();
                };
            } else {
                pdesc.cameraOverride           = camera_override;
                pdesc.clearBeforeDraw          = clear_before_draw;
                pdesc.forceAlphaWrite          = force_alpha_write;
                pdesc.premultipliedSourceBlend = premultiplied_source_blend;
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
                    desc.type = ! rg::IsRuntimeRenderTarget(&scene, url)
                                    ? rg::TexNode::TexType::Imported
                                    : rg::TexNode::TexType::Temp;
                    input     = builder.createTexNode(desc);
                    if (rg::IsRuntimeRenderTarget(&scene, url)) builder.markVirtualWrite(input);
                    if (sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER))
                        extra.use_mipmap_framebuffer = true;
                }

                if (url == output) {
                    builder.markSelfWrite(input);
                    input = rg::addCopyPass(rgraph, input);
                }
                builder.read(input);
                pdesc.textures.emplace_back(input->key());
            }

            rg::TexNode* output_node { nullptr };
            output_node =
                builder.createTexNode(rg::TexNode::Desc { .name = output.data(),
                                                          .key  = output.data(),
                                                          .type = rg::TexNode::TexType::Temp },
                                      true);
            builder.write(output_node);
            if (output == SpecTex_Default) {
                extra.id_link_map[(usize)imgId] = output_node;
            }
        });

    // load effect
    if (imgeff != nullptr) loadEffect(imgeff);
}

std::unique_ptr<rg::RenderGraph> wallpaper::BuildWESceneRenderPlan(Scene& scene) {
    std::unique_ptr<rg::RenderGraph> rgraph = std::make_unique<rg::RenderGraph>();
    ExtraInfo                        extra { .rgraph = rgraph.get(), .scene = &scene };
    TraverseNode(
        [&extra](SceneNode* node) {
            ToGraphPass(node, SpecTex_Default, node->ID(), extra);
        },
        scene.sceneGraph.get());

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

    if (extra.use_mipmap_framebuffer) {
        rg::addCopyPass(*rgraph,
                        rg::TexNode::Desc { .name = SpecTex_Default.data(),
                                            .key  = SpecTex_Default.data(),
                                            .type = rg::TexNode::TexType::Temp },
                        rg::TexNode::Desc { .name = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                            .key  = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                            .type = rg::TexNode::TexType::Temp });
    }

    return rgraph;
}
