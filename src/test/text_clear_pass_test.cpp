#include "render/rendergraph/include/rendergraph/RenderGraph.hpp"
#include "render/vulkanrender/ClearPass.hpp"
#include "render/vulkanrender/TextPass.hpp"

#include <cassert>

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

    return 0;
}
