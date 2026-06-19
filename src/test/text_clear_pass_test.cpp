#include "render/rendergraph/include/rendergraph/RenderGraph.hpp"
#include "render/vulkanrender/ClearPass.hpp"
#include "render/vulkanrender/CopyPass.hpp"
#include "render/vulkanrender/CustomShaderPass.hpp"
#include "render/vulkanrender/Resource.hpp"
#include "render/vulkanrender/TextPass.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneNode.h"

#include <array>
#include <cassert>
#include <memory>
#include <string>
#include <unordered_set>

namespace
{
class DeferredProbePass : public wallpaper::vulkan::VulkanPass {
public:
    void prepare(wallpaper::Scene&,
                 const wallpaper::vulkan::Device&,
                 wallpaper::vulkan::RenderingResources&) override {
        prepare_count++;
        setPrepared();
    }
    void execute(const wallpaper::vulkan::Device&,
                 wallpaper::vulkan::RenderingResources&) override {}
    void destory(const wallpaper::vulkan::Device&,
                 wallpaper::vulkan::RenderingResources&) override {
        setPrepared(false);
    }

    int prepare_count { 0 };
};
} // namespace

int main() {
    wallpaper::rg::RenderGraph graph;

    auto* clear_node = graph.addPass<wallpaper::vulkan::ClearPass>(
        "clear",
        wallpaper::rg::PassNode::Type::Clear,
        [](wallpaper::rg::RenderGraphBuilder& builder,
           wallpaper::vulkan::ClearPass::Desc& desc) {
            auto* target = builder.createTexNode(
                wallpaper::rg::TexNode::Desc {
                    .name = "_rt_text",
                    .key = "_rt_text",
                    .type = wallpaper::rg::TexNode::TexType::Temp,
                },
                true);
            builder.write(target);
            desc.target = target->key();
            desc.clear_value = VkClearValue { .color = { 0.0f, 0.0f, 0.0f, 0.0f } };
        });

    auto* text_node = graph.addPass<wallpaper::vulkan::TextPass>(
        "text",
        wallpaper::rg::PassNode::Type::Text,
        [](wallpaper::rg::RenderGraphBuilder& builder,
           wallpaper::vulkan::TextPass::Desc& desc) {
            auto* target = builder.createTexNode(
                wallpaper::rg::TexNode::Desc {
                    .name = "_rt_text",
                    .key = "_rt_text",
                    .type = wallpaper::rg::TexNode::TexType::Temp,
                },
                true);
            builder.write(target);
            desc.layer_id = 7;
            desc.output = target->key();
        });

    assert(clear_node != nullptr);
    assert(text_node != nullptr);
    assert(clear_node->type() == wallpaper::rg::PassNode::Type::Clear);
    assert(text_node->type() == wallpaper::rg::PassNode::Type::Text);

    auto* clear_pass = dynamic_cast<wallpaper::vulkan::ClearPass*>(
        graph.getPass(clear_node->ID()));
    auto* text_pass = dynamic_cast<wallpaper::vulkan::TextPass*>(
        graph.getPass(text_node->ID()));
    assert(clear_pass != nullptr);
    assert(text_pass != nullptr);
    assert(clear_pass->desc().target == "_rt_text");
    assert(text_pass->desc().output == "_rt_text");
    assert(text_pass->desc().layer_id == 7);
    auto original_text_ref = graph.getPassShared(text_node->ID());
    assert(original_text_ref != nullptr);
    wallpaper::SceneNode replacement_node;
    wallpaper::vulkan::TextPass::Desc replacement_desc;
    replacement_desc.node = &replacement_node;
    replacement_desc.layer_id = 7;
    replacement_desc.output = "_rt_text_replaced";
    auto replacement_ref = std::make_shared<wallpaper::vulkan::TextPass>(replacement_desc);
    assert(graph.replacePass(text_node->ID(), replacement_ref));
    assert(graph.getPassShared(text_node->ID()) == replacement_ref);
    assert(graph.getPass(text_node->ID()) == replacement_ref.get());
    assert(graph.replacePass(text_node->ID(), original_text_ref));
    text_pass = dynamic_cast<wallpaper::vulkan::TextPass*>(
        graph.getPass(text_node->ID()));
    assert(text_pass != nullptr);
    assert(clear_pass->residencyKey() == "ClearPass|target=_rt_text");
    assert(text_pass->residencyKey().find("TextPass|node=") == 0);
    assert(text_pass->residencyKey().find("|layer=7|output=_rt_text") != std::string::npos);
    assert(text_pass->referencesRenderTarget("_rt_text"));
    assert(!text_pass->referencesRenderTarget("_rt_other"));
    assert(text_pass->referencesTextLayer(7));
    assert(!text_pass->referencesTextLayer(0));
    assert(!text_pass->referencesTextLayer(8));
    assert(text_pass->referencesAnyRenderTarget(std::unordered_set<std::string> { "_rt_other",
                                                                                  "_rt_text" }));
    assert(!text_pass->referencesAnyRenderTarget(std::unordered_set<std::string> { "_rt_other" }));
    assert(text_pass->referencesAnyTextLayer(std::unordered_set<int32_t> { 3, 7 }));
    assert(!text_pass->referencesAnyTextLayer(std::unordered_set<int32_t> { 3, 8 }));
    assert(!text_pass->prepared());
    wallpaper::vulkan::Device device;
    wallpaper::vulkan::RenderingResources resources;
    text_pass->execute(device, resources);
    assert(!text_pass->prepared());

    wallpaper::vulkan::ClearPass::Desc clear_desc;
    clear_desc.target = "_rt_text";
    wallpaper::vulkan::ClearPass matching_clear(clear_desc);
    clear_desc.target = "_rt_other";
    wallpaper::vulkan::ClearPass other_clear(clear_desc);
    assert(clear_pass->canReuseForResidency(matching_clear));
    assert(!clear_pass->canReuseForResidency(other_clear));
    assert(clear_pass->referencesRenderTarget("_rt_text"));
    assert(!clear_pass->referencesRenderTarget("_rt_other"));

    wallpaper::vulkan::CopyPass::Desc copy_desc;
    copy_desc.src = "_rt_a";
    copy_desc.dst = "_rt_b";
    bool copy_should_execute = true;
    copy_desc.should_execute = [&copy_should_execute] { return copy_should_execute; };
    wallpaper::vulkan::CopyPass copy(copy_desc);
    wallpaper::vulkan::CopyPass matching_copy(copy_desc);
    copy_desc.src = "_rt_b";
    copy_desc.dst = "_rt_a";
    wallpaper::vulkan::CopyPass other_copy(copy_desc);
    assert(copy.residencyKey() == "CopyPass|src=_rt_a|dst=_rt_b");
    assert(copy.canReuseForResidency(matching_copy));
    assert(!copy.canReuseForResidency(other_copy));
    assert(copy.referencesRenderTarget("_rt_a"));
    assert(copy.referencesRenderTarget("_rt_b"));
    assert(!copy.referencesRenderTarget("_rt_c"));
    bool replacement_should_execute = false;
    wallpaper::vulkan::CopyPass::Desc copy_gate_desc;
    copy_gate_desc.src = "_rt_a";
    copy_gate_desc.dst = "_rt_b";
    copy_gate_desc.should_execute = [&replacement_should_execute] {
        return replacement_should_execute;
    };
    wallpaper::vulkan::CopyPass replacement_gate(copy_gate_desc);
    copy.absorbResidencyGraphState(replacement_gate);
    assert(copy.desc().should_execute);
    assert(!copy.desc().should_execute());
    replacement_should_execute = true;
    assert(copy.desc().should_execute());

    wallpaper::SceneNode text_node_state;
    wallpaper::vulkan::TextPass::Desc text_desc;
    text_desc.node = &text_node_state;
    text_desc.layer_id = 11;
    text_desc.output = "_rt_text_a";
    wallpaper::vulkan::TextPass text_a(text_desc);
    wallpaper::vulkan::TextPass matching_text(text_desc);
    text_desc.output = "_rt_text_b";
    wallpaper::vulkan::TextPass other_text(text_desc);
    assert(text_a.canReuseForResidency(matching_text));
    assert(!text_a.canReuseForResidency(other_text));
    text_a.absorbResidencyGraphState(other_text);
    assert(text_a.desc().output == "_rt_text_b");
    assert(text_a.referencesRenderTarget("_rt_text_b"));

    wallpaper::SceneNode shader_node;
    wallpaper::vulkan::CustomShaderPass::Desc shader_desc;
    shader_desc.node = &shader_node;
    shader_desc.output = "_rt_shader";
    wallpaper::vulkan::CustomShaderPass shader(shader_desc);
    wallpaper::vulkan::CustomShaderPass matching_shader(shader_desc);
    shader_desc.output = "_rt_other";
    wallpaper::vulkan::CustomShaderPass other_shader(shader_desc);
    assert(shader.residencyKey().find("CustomShaderPass|layer=") == 0);
    assert(shader.residencyKey().find("|output=_rt_shader") != std::string::npos);
    assert(shader.canReuseForResidency(matching_shader));
    assert(!shader.canReuseForResidency(other_shader));

    wallpaper::Scene deferred_scene;
    DeferredProbePass deferred_probe;
    assert(deferred_probe.requestDeferredPrepareResources(deferred_scene, device) ==
           wallpaper::vulkan::DeferredPrepareResourcesState::Ready);
    assert(!deferred_probe.prepared());
    deferred_probe.prepareDeferred(deferred_scene, device, resources);
    assert(deferred_probe.prepared());
    assert(deferred_probe.prepare_count == 1);
    deferred_probe.clearReleaseTexs();
    deferred_probe.addReleaseTexs(std::array<std::string_view, 3> {
        "_rt_a",
        "_rt_b",
        "_rt_a",
    });
    assert(deferred_probe.releaseTexs().size() == 2);
    assert(deferred_probe.releaseTexs()[0] == "_rt_a");
    assert(deferred_probe.releaseTexs()[1] == "_rt_b");

    return 0;
}
