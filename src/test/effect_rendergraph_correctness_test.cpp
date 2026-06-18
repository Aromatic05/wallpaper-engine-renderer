#include "backend/scene/internal/engine/WESceneRenderPlanBuilder.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneCamera.h"
#include "backend/scene/internal/scene/include/scene/SceneImageEffectLayer.h"
#include "backend/scene/internal/scene/include/scene/SceneMesh.h"
#include "render/vulkanrender/CopyPass.hpp"
#include "render/vulkanrender/CustomShaderPass.hpp"
#include "render/vulkanrender/PassCommon.hpp"
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

std::unique_ptr<Fixture> makeFixture(bool fullscreen) {
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
    });

    auto nodeB = makeNode("effect-b", { std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_A) });
    fx->effectB = std::make_shared<SceneImageEffect>();
    fx->effectB->SetLocalVisible(true);
    fx->effectB->nodes.push_back(SceneImageEffectNode {
        .authored_output = std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_B),
        .output = std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_B),
        .authored_textures = { std::string(wallpaper::WE_EFFECT_PPONG_PREFIX_A) },
        .sceneNode = nodeB,
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
                            desc.cameraOverride);
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
} // namespace

int main() {
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
        assert(normalFinal->cameraOverride.empty());

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
        assert(authoredFinal->output == fixture->pingA);
        assert(findCopy(*graph, fixture->pingA, SpecTex_Default.data()) == nullptr);
        assert(finalPass->premultipliedSourceBlend);
        assert(authoredFinal->clearBeforeDraw);
        assert(authoredFinal->forceAlphaWrite);

        VkPipelineColorBlendAttachmentState blend {};
        vk::SetBlend(BlendMode::Translucent, blend, false);
        assert(blend.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        assert(blend.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        assert(blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
        assert(blend.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

        vk::SetBlend(BlendMode::Translucent, blend, true);
        assert(blend.srcColorBlendFactor == VK_BLEND_FACTOR_ONE);
        assert(blend.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        assert(blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE);

        vk::SetBlend(BlendMode::Additive, blend, false);
        assert(blend.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        assert(blend.dstColorBlendFactor == VK_BLEND_FACTOR_ONE);
        assert(blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
        assert(blend.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
    }

    return 0;
}
