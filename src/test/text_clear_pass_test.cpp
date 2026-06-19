#include "render/rendergraph/include/rendergraph/RenderGraph.hpp"
#include "render/vulkanrender/ClearPass.hpp"
#include "render/vulkanrender/CopyPass.hpp"
#include "render/vulkanrender/CustomShaderPass.hpp"
#include "render/vulkanrender/Resource.hpp"
#include "render/vulkanrender/TextPass.hpp"
#include "backend/scene/internal/scene/include/scene/SceneNode.h"

#include <cassert>
#include <string>

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
    assert(clear_pass->residencyKey() == "ClearPass|target=_rt_text");
    assert(text_pass->residencyKey().find("TextPass|node=") == 0);
    assert(text_pass->residencyKey().find("|layer=7|output=_rt_text") != std::string::npos);
    assert(!text_pass->prepared());
    wallpaper::vulkan::Device device;
    wallpaper::vulkan::RenderingResources resources;
    text_pass->execute(device, resources);
    assert(text_pass->executionDiagnosticEmitted());
    assert(!text_pass->prepared());

    wallpaper::vulkan::ClearPass::Desc clear_desc;
    clear_desc.target = "_rt_text";
    wallpaper::vulkan::ClearPass matching_clear(clear_desc);
    clear_desc.target = "_rt_other";
    wallpaper::vulkan::ClearPass other_clear(clear_desc);
    assert(clear_pass->canReuseForResidency(matching_clear));
    assert(!clear_pass->canReuseForResidency(other_clear));

    wallpaper::vulkan::CopyPass::Desc copy_desc;
    copy_desc.src = "_rt_a";
    copy_desc.dst = "_rt_b";
    wallpaper::vulkan::CopyPass copy(copy_desc);
    wallpaper::vulkan::CopyPass matching_copy(copy_desc);
    copy_desc.src = "_rt_b";
    copy_desc.dst = "_rt_a";
    wallpaper::vulkan::CopyPass other_copy(copy_desc);
    assert(copy.residencyKey() == "CopyPass|src=_rt_a|dst=_rt_b");
    assert(copy.canReuseForResidency(matching_copy));
    assert(!copy.canReuseForResidency(other_copy));

    wallpaper::SceneNode shader_node;
    wallpaper::vulkan::CustomShaderPass::Desc shader_desc;
    shader_desc.node = &shader_node;
    shader_desc.output = "_rt_shader";
    wallpaper::vulkan::CustomShaderPass shader(shader_desc);
    wallpaper::vulkan::CustomShaderPass matching_shader(shader_desc);
    shader_desc.output = "_rt_other";
    wallpaper::vulkan::CustomShaderPass other_shader(shader_desc);
    assert(shader.residencyKey().find("CustomShaderPass|node=") == 0);
    assert(shader.residencyKey().find("|output=_rt_shader") != std::string::npos);
    assert(shader.canReuseForResidency(matching_shader));
    assert(!shader.canReuseForResidency(other_shader));

    return 0;
}
