#include "backend/scene/internal/engine/WESceneRenderPlanBuilder.hpp"
#include "backend/scene/internal/parser/WPShaderParser.hpp"
#include "backend/scene/internal/parser/effect/FinalComposite.hpp"
#include "backend/scene/internal/wpscene/WPMaterial.h"
#include "common/fs/include/fs/VFS.h"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneCamera.h"
#include "backend/scene/internal/scene/include/scene/SceneImageEffectLayer.h"
#include "backend/scene/internal/scene/include/scene/SceneMesh.h"
#include "backend/scene/internal/scene/include/scene/SceneTextPrimitive.h"
#include "render/vulkanrender/ClearPass.hpp"
#include "render/vulkanrender/CopyPass.hpp"
#include "render/vulkanrender/CustomShaderPass.hpp"
#include "render/vulkanrender/PassCommon.hpp"
#include "render/vulkanrender/TextPass.hpp"
#include "rendergraph/RenderGraph.hpp"
#include "backend/scene/internal/SpecTexs.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
using wallpaper::BlendMode;
using wallpaper::Scene;
using wallpaper::SceneCamera;
using wallpaper::SceneImageEffect;
using wallpaper::SceneImageEffectLayer;
using wallpaper::SceneImageEffectNode;
using wallpaper::SceneMaterial;
using wallpaper::SceneMesh;
using wallpaper::SceneNode;
using wallpaper::SpecTex_Default;
namespace rg = wallpaper::rg;
namespace vk = wallpaper::vulkan;

std::shared_ptr<SceneNode> makeNode(const std::string& name, std::vector<std::string> textures) {
    auto node = std::make_shared<SceneNode>();
    auto mesh = std::make_shared<SceneMesh>();
    SceneMaterial material;
    material.name = name;
    material.textures = std::move(textures);
    material.blenmode = BlendMode::Translucent;
    mesh->AddMaterial(std::move(material));
    node->AddMesh(mesh);
    return node;
}

std::shared_ptr<SceneMesh> makeMesh(const std::string& name, std::vector<std::string> textures,
                                    BlendMode blend = BlendMode::Translucent) {
    auto mesh = std::make_shared<SceneMesh>();
    SceneMaterial material;
    material.name     = name;
    material.textures = std::move(textures);
    material.blenmode = blend;
    mesh->AddMaterial(std::move(material));
    return mesh;
}

struct Fixture {
    Scene                                      scene;
    std::shared_ptr<SceneNode>                 owner;
    std::shared_ptr<SceneImageEffectLayer>     layer;
    std::shared_ptr<SceneImageEffect>          effectA;
    std::shared_ptr<SceneImageEffect>          effectB;
    std::string                                pingA;
    std::string                                pingB;
};

std::unique_ptr<Fixture> makeFixture(bool fullscreen, bool final_can_composite = true) {
    auto fx = std::make_unique<Fixture>();
    fx->pingA = std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_A) + "fixture";
    fx->pingB = std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_B) + "fixture";

    fx->scene.renderTargets[SpecTex_Default.data()] = { 64, 64, true };
    fx->scene.renderTargets[fx->pingA]              = { 64, 64, true };
    fx->scene.renderTargets[fx->pingB]              = { 64, 64, true };

    auto camNode = std::make_shared<SceneNode>();
    fx->scene.cameras["effect"] = std::make_shared<SceneCamera>(2, 2, -1.0f, 1.0f);
    fx->scene.cameras["effect"]->AttatchNode(camNode);

    fx->scene.cameras["layercam"] = std::make_shared<SceneCamera>(64, 64, -1.0f, 1.0f);
    fx->scene.cameras["layercam"]->AttatchNode(std::make_shared<SceneNode>());

    fx->owner = makeNode("source", { "source.png" });
    fx->owner->SetCamera("layercam");
    fx->layer = std::make_shared<SceneImageEffectLayer>(
        fx->owner.get(), 64.0f, 64.0f, fx->pingA, fx->pingB);
    fx->layer->SetFinalBlend(BlendMode::Translucent);
    fx->layer->SetFullscreen(fullscreen);
    fx->layer->FinalNode().CopyTrans(*fx->owner);
    fx->layer->FinalMesh().ChangeMeshDataFrom(fx->scene.default_effect_mesh);
    fx->layer->FinalNode().AddMesh(
        makeMesh("final-composite", { fx->pingA }, BlendMode::Translucent));
    fx->scene.cameras["layercam"]->AttatchImgEffect(fx->layer);

    auto nodeA = makeNode("effect-a", { std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_A) });
    fx->effectA = std::make_shared<SceneImageEffect>();
    fx->effectA->SetLocalVisible(true);
    fx->effectA->nodes.push_back(SceneImageEffectNode {
        .authored_output = std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_B),
        .output = std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_B),
        .authored_textures = { std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_A) },
        .sceneNode = nodeA,
        .can_composite_final = false,
    });

    auto nodeB = makeNode("effect-b", { std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_A) });
    fx->effectB = std::make_shared<SceneImageEffect>();
    fx->effectB->SetLocalVisible(true);
    fx->effectB->nodes.push_back(SceneImageEffectNode {
        .authored_output = std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_B),
        .output = std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_B),
        .authored_textures = { std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_A) },
        .sceneNode = nodeB,
        .can_composite_final = final_can_composite,
    });

    fx->layer->AddEffect(fx->effectA);
    fx->layer->AddEffect(fx->effectB);
    fx->scene.sceneGraph->AppendChild(fx->owner);
    return fx;
}

std::vector<std::string> snapshot(rg::RenderGraph& graph) {
    std::vector<std::string> lines;
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        if (auto* shader = dynamic_cast<vk::CustomShaderPass*>(pass)) {
            const auto& desc = shader->desc();
            const auto  tex0 = desc.textures.empty() ? std::string() : desc.textures.front();
            const auto* material = desc.node != nullptr && desc.node->Mesh() != nullptr
                                       ? desc.node->Mesh()->Material()
                                       : nullptr;
            const auto material_name = material != nullptr ? material->name : std::string();
            lines.push_back("shader:" + material_name + ":" + desc.output + ":" + tex0 + ":" +
                            desc.camera_override);
        } else if (auto* copy = dynamic_cast<vk::CopyPass*>(pass)) {
            const auto& desc = copy->desc();
            lines.push_back("copy:" + desc.src + ":" + desc.dst);
        }
    }
    return lines;
}

const vk::CustomShaderPass::Desc* findShader(rg::RenderGraph& graph, const std::string& output) {
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        if (auto* shader = dynamic_cast<vk::CustomShaderPass*>(pass)) {
            if (shader->desc().output == output) return &shader->desc();
        }
    }
    return nullptr;
}

const vk::CopyPass::Desc* findCopy(rg::RenderGraph& graph, const std::string& src,
                                   const std::string& dst) {
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        if (auto* copy = dynamic_cast<vk::CopyPass*>(pass)) {
            if (copy->desc().src == src && copy->desc().dst == dst) return &copy->desc();
        }
    }
    return nullptr;
}

const vk::CustomShaderPass::Desc* findShaderByMaterial(rg::RenderGraph& graph,
                                                       const std::string& material_name) {
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        if (auto* shader = dynamic_cast<vk::CustomShaderPass*>(pass)) {
            const auto* material =
                shader->desc().node != nullptr && shader->desc().node->Mesh() != nullptr
                    ? shader->desc().node->Mesh()->Material()
                    : nullptr;
            if (material != nullptr && material->name == material_name) return &shader->desc();
        }
    }
    return nullptr;
}

vk::TextPass* findTextPass(rg::RenderGraph& graph, int32_t layer_id,
                           const std::string& output) {
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        if (auto* text = dynamic_cast<vk::TextPass*>(pass)) {
            if (text->desc().layer_id == layer_id && text->desc().output == output) {
                return text;
            }
        }
    }
    return nullptr;
}

const vk::ClearPass::Desc* findClearPass(rg::RenderGraph& graph, const std::string& target) {
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        if (auto* clear = dynamic_cast<vk::ClearPass*>(pass)) {
            if (clear->desc().target == target) return &clear->desc();
        }
    }
    return nullptr;
}
} // namespace

int main() {
    {
        wallpaper::wpscene::WPMaterial material;
        wallpaper::WPShaderInfo shader_info;

        for (const std::string shader : { "genericimage", "genericimage2", "genericimage3",
                                          "genericimage4", "passthrough", "effects/transform",
                                          "effects/scroll", "effects/perspective" }) {
            material.shader = shader;
            assert(wallpaper::CanCompositeFinalEffectMaterial(material, shader_info));
        }

        material.shader = "effects/blur";
        assert(! wallpaper::CanCompositeFinalEffectMaterial(material, shader_info));
        shader_info.combos["TRANSPARENCY"] = "1";
        assert(! wallpaper::CanCompositeFinalEffectMaterial(material, shader_info));
        shader_info.textureMaterials["g_Texture0"] = "previous";
        assert(wallpaper::CanCompositeFinalEffectMaterial(material, shader_info));

        wallpaper::fs::VFS vfs;
        wallpaper::WPShaderInfo parsed_info;
        std::vector<wallpaper::WPShaderTexInfo> texture_info(1);
        texture_info.front().enabled = true;
        const auto parsed_source = wallpaper::WPShaderParser::PreShaderSrc(
            vfs,
            R"(
                // [COMBO] {"combo":"TRANSPARENCY","default":1}
                uniform sampler2D g_Texture0; // {"material":"previous"}
            )",
            &parsed_info,
            texture_info);
        assert(! parsed_source.empty());
        assert(parsed_info.combos.count("TRANSPARENCY") == 1);
        assert(parsed_info.textureMaterials.at("g_Texture0") == "previous");
        assert(wallpaper::CanCompositeFinalEffectMaterial(material, parsed_info));
    }

    {
        auto direct = makeFixture(false, true);
        auto graph = wallpaper::BuildWESceneRenderPlan(direct->scene);
        auto* authored = findShaderByMaterial(*graph, "effect-b");
        auto* neutral = findShaderByMaterial(*graph, "final-composite");
        assert(direct->layer->AuthoredFinalCanComposite());
        assert(! direct->layer->PublishesPrivateFinalComposite());
        assert(authored != nullptr && authored->output == SpecTex_Default);
        assert(neutral != nullptr && neutral->should_execute && ! neutral->should_execute());
    }

    {
        auto private_final = makeFixture(false, false);
        auto graph = wallpaper::BuildWESceneRenderPlan(private_final->scene);
        auto* authored = findShaderByMaterial(*graph, "effect-b");
        auto* neutral = findShaderByMaterial(*graph, "final-composite");
        assert(! private_final->layer->AuthoredFinalCanComposite());
        assert(private_final->layer->PublishesPrivateFinalComposite());
        assert(authored != nullptr && authored->output != SpecTex_Default);
        assert(neutral != nullptr && neutral->output == SpecTex_Default);
        assert(neutral->should_execute && neutral->should_execute());
        assert(neutral->textures.front() == private_final->layer->ResolvedPrivateOutputTarget());
    }

    {
        auto fixture   = makeFixture(false);
        auto graphA    = wallpaper::BuildWESceneRenderPlan(fixture->scene);
        auto firstSnap = snapshot(*graphA);
        auto graphB    = wallpaper::BuildWESceneRenderPlan(fixture->scene);
        auto secondSnap = snapshot(*graphB);
        auto graphC     = wallpaper::BuildWESceneRenderPlan(fixture->scene);
        auto thirdSnap  = snapshot(*graphC);

        assert(firstSnap == secondSnap);
        assert(secondSnap == thirdSnap);
        assert(fixture->effectA->nodes.front().authored_output ==
               wallpaper::WE_EFFECT_PPONG_PREFIX_B);
        assert(fixture->effectB->nodes.front().authored_output ==
               wallpaper::WE_EFFECT_PPONG_PREFIX_B);
    }

    {
        auto fixture = makeFixture(false);
        auto check = [&](bool visible) {
            fixture->effectA->SetLocalVisible(visible);
            auto graph = wallpaper::BuildWESceneRenderPlan(fixture->scene);
            auto* bypass = findCopy(*graph, fixture->pingA, fixture->pingB);
            assert(bypass != nullptr);
            assert(static_cast<bool>(bypass->should_execute) == true);
            assert(bypass->should_execute() == !visible);
            auto* finalComposite = findShaderByMaterial(*graph, "final-composite");
            assert(finalComposite != nullptr);
            assert(finalComposite->output == SpecTex_Default);
            assert(finalComposite->textures.front() == fixture->pingA);
        };

        check(true);
        check(false);
        check(false);
        check(true);
    }

    {
        auto fixture = makeFixture(false);
        fixture->effectA->SetLocalVisible(false);
        fixture->effectB->SetLocalVisible(true);
        auto graphHideA = wallpaper::BuildWESceneRenderPlan(fixture->scene);
        auto* bypassA = findCopy(*graphHideA, fixture->pingA, fixture->pingB);
        auto* finalA  = findShaderByMaterial(*graphHideA, "final-composite");
        assert(bypassA != nullptr && bypassA->should_execute());
        assert(finalA != nullptr);
        assert(finalA->textures.front() == fixture->pingA);

        fixture->effectA->SetLocalVisible(true);
        fixture->effectB->SetLocalVisible(false);
        auto graphHideB = wallpaper::BuildWESceneRenderPlan(fixture->scene);
        auto* bypassB = findCopy(*graphHideB, fixture->pingB, fixture->pingA);
        auto* finalB  = findShaderByMaterial(*graphHideB, "final-composite");
        assert(bypassB != nullptr);
        assert(bypassB->should_execute());
        assert(finalB != nullptr);
        assert(finalB->textures.front() == fixture->pingA);
    }

    {
        auto normal = makeFixture(false);
        auto normalGraph = wallpaper::BuildWESceneRenderPlan(normal->scene);
        auto* normalFinal = findShaderByMaterial(*normalGraph, "final-composite");
        assert(normalFinal != nullptr);
        assert(normalFinal->camera_override.empty());

        auto fullscreen = makeFixture(true);
        auto fullscreenGraph = wallpaper::BuildWESceneRenderPlan(fullscreen->scene);
        auto* fullscreenFinal = findShaderByMaterial(*fullscreenGraph, "final-composite");
        assert(fullscreenFinal != nullptr);
        assert(fullscreen->layer->FinalNode().Camera() == "effect");
    }

    {
        auto fixture = makeFixture(false);
        auto graph = wallpaper::BuildWESceneRenderPlan(fixture->scene);
        auto* finalPass = findShaderByMaterial(*graph, "final-composite");
        auto* authoredFinal = findShaderByMaterial(*graph, "effect-b");
        assert(finalPass != nullptr);
        assert(authoredFinal != nullptr);
        assert(finalPass->output == SpecTex_Default);
        assert(authoredFinal->output == SpecTex_Default);
        assert(findCopy(*graph, fixture->pingA, SpecTex_Default.data()) == nullptr);
        assert(finalPass->premultiplied_source_blend);

        VkPipelineColorBlendAttachmentState blend {};
        vk::SetBlend(BlendMode::Translucent, blend);
        assert(blend.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        assert(blend.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        assert(blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        assert(blend.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

        vk::SetBlend(BlendMode::Translucent, blend);
        assert(blend.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        assert(blend.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        assert(blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);

        vk::SetBlend(BlendMode::Additive, blend);
        assert(blend.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        assert(blend.dstColorBlendFactor == VK_BLEND_FACTOR_ONE);
        assert(blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        assert(blend.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
    }

    {
        Scene scene;
        scene.renderTargets[SpecTex_Default.data()] = { 64, 64, true };
        auto node = std::make_shared<SceneNode>();
        node->ID() = 77;
        scene.sceneGraph->AppendChild(node);
        auto primitive = std::make_shared<wallpaper::SceneTextPrimitive>();
        primitive->object.id = 77;
        node->AddText(primitive);
        scene.textPrimitives[77] = primitive;

        auto graph = wallpaper::BuildWESceneRenderPlan(scene);
        auto* text = findTextPass(*graph, 77, SpecTex_Default.data());
        assert(text != nullptr);
        assert(text->desc().node == node.get());
        assert(text->referencesTextLayer(77));
        assert(! text->referencesTextLayer(88));
    }

    {
        Scene scene;
        const std::string bridge_target = "_rt_text_bridge";
        scene.renderTargets[SpecTex_Default.data()] = { 64, 64, true };
        scene.renderTargets[bridge_target] = { 32, 16, true };
        auto node = std::make_shared<SceneNode>();
        node->ID() = 88;
        scene.sceneGraph->AppendChild(node);
        auto primitive = std::make_shared<wallpaper::SceneTextPrimitive>();
        primitive->object.id = 88;
        node->AddText(primitive);
        primitive->bridge.enabled    = true;
        primitive->bridge.pingpong_a = bridge_target;
        primitive->bridge.pingpong_b = "_rt_text_bridge_b";
        primitive->bridge.render_targets.push_back(
            wallpaper::TextBridgeRenderTarget { .name = bridge_target, .scale = 1 });
        scene.renderTargets[primitive->bridge.pingpong_b] = { 32, 16, true };
        const std::string camera_name = "text-bridge-camera";
        node->SetCamera(camera_name);
        auto camera = std::make_shared<wallpaper::SceneCamera>(32, 16, -1.0f, 1.0f);
        camera->AttatchNode(node);
        camera->AttatchImgEffect(std::make_shared<wallpaper::SceneImageEffectLayer>(
            node.get(), 32.0f, 16.0f, bridge_target, primitive->bridge.pingpong_b));
        scene.cameras[camera_name] = std::move(camera);
        scene.textPrimitives[88] = primitive;

        auto graph = wallpaper::BuildWESceneRenderPlan(scene);
        assert(findClearPass(*graph, bridge_target) == nullptr);
        auto* bridged_text = findTextPass(*graph, 88, bridge_target);
        assert(bridged_text != nullptr);
        assert(bridged_text->desc().clear_output);
        assert(bridged_text->referencesTextLayer(88));
        assert(! bridged_text->referencesTextLayer(77));
    }

    {
        Scene scene;
        scene.renderTargets[SpecTex_Default.data()] = { 64, 64, true };

        auto hidden_parent = makeNode("hidden-particle-parent", { "parent.png" });
        hidden_parent->ID() = 501;
        hidden_parent->SetVisible(false);

        auto generated_child = makeNode("generated-particle-child", { "child.png" });
        generated_child->ID() = -1;
        hidden_parent->AppendChild(generated_child);
        scene.sceneGraph->AppendChild(hidden_parent);

        auto hidden_graph = wallpaper::BuildWESceneRenderPlan(scene);
        assert(findShaderByMaterial(*hidden_graph, "hidden-particle-parent") == nullptr);
        assert(findShaderByMaterial(*hidden_graph, "generated-particle-child") == nullptr);

        hidden_parent->SetVisible(true);
        auto visible_graph = wallpaper::BuildWESceneRenderPlan(scene);
        assert(findShaderByMaterial(*visible_graph, "hidden-particle-parent") != nullptr);
        assert(findShaderByMaterial(*visible_graph, "generated-particle-child") != nullptr);
    }

    return 0;
}
