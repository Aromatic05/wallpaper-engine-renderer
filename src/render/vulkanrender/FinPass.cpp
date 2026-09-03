#include "FinPass.hpp"
#include "vulkan/Shader.hpp"
#include "Resource.hpp"
#include "PassCommon.hpp"

using namespace wallpaper::vulkan;

constexpr std::string_view vert_code = R"(
struct VSInput {
    [[vk::location(0)]] float3 Position : POSITION0;
    [[vk::location(1)]] float2 Texcoord : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 texcoord : TEXCOORD0;
};

VSOutput main_vs(VSInput input) {
    VSOutput output;
    output.texcoord = input.Texcoord;
    output.position = float4(input.Position, 1.0);
    return output;
}
)";

constexpr std::string_view frag_code = R"(
struct PSInput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 texcoord : TEXCOORD0;
};

[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float4> u_Texture;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState u_Texture_ww_sampler;

float4 main_ps(PSInput input) : SV_Target0 {
    return u_Texture.Sample(u_Texture_ww_sampler, input.texcoord);
}
)";

struct VertexInput {
    std::array<float, 3> pos;
    std::array<float, 2> color;
};

constexpr std::array vertex_input = {
    VertexInput { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
    VertexInput { { -1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
    VertexInput { { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } },
    VertexInput { { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
};

FinPass::FinPass(const Desc& desc): m_sample_count(desc.sample_count) {}
FinPass::~FinPass() {}

bool FinPass::referencesRenderTarget(std::string_view render_target) const {
    // The final present pass samples only the render-graph result target. Text bridge target
    // resizes never need to rebind this pass unless the default result image itself was recreated.
    return m_desc.result == render_target;
}
namespace
{
std::optional<VmaImageParameters> CreateMultisampleColorImage(
    const Device& device,
    VkExtent3D extent,
    VkFormat format,
    VkSampleCountFlagBits sample_count) {
    VmaImageParameters image;
    VkImageCreateInfo info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = extent,
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = sample_count,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    image.extent = extent;

    VmaAllocationCreateInfo vma_info {};
    vma_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VVK_CHECK_ACT(return std::nullopt,
                  vvk::CreateImage(device.vma_allocator(), info, vma_info, image.handle));

    VkImageViewCreateInfo view_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .image = *image.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {},
        .subresourceRange =
            VkImageSubresourceRange {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    VVK_CHECK_ACT(return std::nullopt,
                  device.handle().CreateImageView(view_info, image.view));
    return image;
}

bool EnsureMultisampleColorImage(const Device& device,
                                 VkExtent3D extent,
                                 VkFormat format,
                                 VkSampleCountFlagBits sample_count,
                                 VmaImageParameters& image) {
    if (sample_count == VK_SAMPLE_COUNT_1_BIT) return true;
    if (image.handle && image.extent.width == extent.width &&
        image.extent.height == extent.height && image.extent.depth == extent.depth) {
        return true;
    }

    auto next = CreateMultisampleColorImage(device, extent, format, sample_count);
    if (! next.has_value()) return false;
    image = std::move(*next);
    return true;
}

std::optional<vvk::RenderPass> CreateRenderPass(const vvk::Device& device,
                                                VkFormat format,
                                                VkImageLayout final_layout,
                                                VkSampleCountFlagBits sample_count) {
    const auto plan = BuildFinalOutputAttachmentPlan(sample_count);
    std::array<VkAttachmentDescription, 2> attachments {};
    attachments[0] = VkAttachmentDescription {
        .format = format,
        .samples = plan.sample_count,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = plan.uses_resolve ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                     : VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = plan.uses_resolve ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                         : final_layout,
    };
    if (plan.uses_resolve) {
        attachments[1] = VkAttachmentDescription {
            .format = format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = final_layout,
        };
    }

    VkAttachmentReference color_ref {
        .attachment = plan.color_attachment,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference resolve_ref {
        .attachment = plan.resolve_attachment,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pResolveAttachments = plan.uses_resolve ? &resolve_ref : nullptr,
    };

    VkRenderPassCreateInfo create_info {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .attachmentCount = plan.attachment_count,
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };
    vvk::RenderPass pass;
    const VkResult result = device.CreateRenderPass(create_info, pass);
    if (result == VK_SUCCESS) return pass;
    VVK_CHECK(result);
    return std::nullopt;
}
} // namespace

void FinPass::setPresent(ImageParameters img) { m_desc.vk_present = img; }
void FinPass::setPresentLayout(VkImageLayout layout) { m_desc.present_layout = layout; }
void FinPass::setPresentFormat(VkFormat format) { m_desc.present_format = format; }
void FinPass::setPresentQueueIndex(uint32_t i) { m_desc.present_queue_index = i; }

void FinPass::prepare(Scene& scene, const Device& device, RenderingResources& rr) {
    {
        auto tex_name = std::string(m_desc.result);
        if (scene.renderTargets.count(tex_name) == 0) return;
        auto& rt = scene.renderTargets.at(tex_name);
        if (auto opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
            opt.has_value()) {
            m_desc.vk_result = opt.value();
        }
    }
    std::vector<Uni_ShaderSpv> spvs;
    {
        ShaderCompOpt opt;
        opt.target_env = ShaderTargetEnv::VULKAN_1_1;

        std::array<ShaderCompUnit, 2> units;
        units[0] = ShaderCompUnit {
            .stage = wallpaper::ShaderType::VERTEX,
            .source_language = ShaderSourceLanguage::HLSL,
            .debug_name = "FinPass.vert",
            .entry_point = "main_vs",
            .src = std::string(vert_code),
        };
        units[1] = ShaderCompUnit {
            .stage = wallpaper::ShaderType::FRAGMENT,
            .source_language = ShaderSourceLanguage::HLSL,
            .debug_name = "FinPass.frag",
            .entry_point = "main_ps",
            .src = std::string(frag_code),
        };
        if (!CompileAndLinkShaderUnits(units, opt, spvs)) {
            LOG_ERROR("FinPass: failed to compile fullscreen present shaders");
            return;
        }
    }

    VkVertexInputBindingDescription                bind_description;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
    {
        bind_description.stride    = (sizeof(VertexInput));
        bind_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bind_description.binding   = (0);
        VkVertexInputAttributeDescription attr_pos, attr_color;
        attr_pos.binding    = (0);
        attr_pos.location   = (0);
        attr_pos.format     = VK_FORMAT_R32G32B32_SFLOAT;
        attr_pos.offset     = offsetof(VertexInput, pos);
        attr_color.binding  = (0);
        attr_color.location = (1);
        attr_color.format   = VK_FORMAT_R32G32_SFLOAT;
        attr_color.offset   = (offsetof(VertexInput, color));

        attr_descriptions.push_back(attr_pos);
        attr_descriptions.push_back(attr_color);

        {
            auto& buf = m_desc.vertex_buf;
            if (!rr.vertex_buf->allocateSubRef(sizeof(decltype(vertex_input)), buf)) return;
            if (!rr.vertex_buf->writeToBuf(buf, { (uint8_t*)vertex_input.data(), buf.size })) return;
        }
    }
    DescriptorSetInfo descriptor_info;
    {
        descriptor_info.push_descriptor = true;
        descriptor_info.bindings.resize(1);
        auto& binding           = descriptor_info.bindings.back();
        binding.binding         = (1);
        binding.descriptorCount = (1);
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    {
        auto opt = CreateRenderPass(
            device.handle(), m_desc.present_format, m_desc.present_layout, m_sample_count);
        if (! opt.has_value()) return;
        auto pass = std::move(opt.value());

        if (! EnsureMultisampleColorImage(
                device,
                VkExtent3D { device.out_extent().width, device.out_extent().height, 1 },
                m_desc.present_format,
                m_sample_count,
                m_msaa_color)) {
            return;
        }

        descriptor_info.push_descriptor = true;
        GraphicsPipeline pipeline;
        pipeline.toDefault();
        pipeline.multisample = BuildFinalOutputMultisampleState(m_sample_count);
        m_desc.pipeline.debug_name = "FinPass";
        pipeline.addDescriptorSetInfo(spanone { descriptor_info })
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
            .addInputBindingDescription(spanone { bind_description })
            .addInputAttributeDescription(attr_descriptions);
        for (auto& spv : spvs) pipeline.addStage(std::move(spv));

        if (! pipeline.create(device, pass, m_desc.pipeline)) return;
    }
    /*
    if(m_desc.present_layout == vk::ImageLayout::ePresentSrcKHR || m_desc.present_layout ==
    vk::ImageLayout::eSharedPresentKHR) m_desc.render_layout = m_desc.present_layout; else
    m_desc.render_layout = vk::ImageLayout::eColorAttachmentOptimal;
    */

    m_desc.render_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    {
        auto& sc           = scene.clearColor;
        m_desc.clear_value = VkClearValue { { sc[0], sc[1], sc[2], 1.0f } };
    }
    setPrepared();
}

void FinPass::refreshResources(Scene& scene, const Device& device, RenderingResources&) {
    // The final composite pass keeps its fullscreen mesh and pipeline across resource-only
    // refreshes. The only scene-owned object it samples is the render-graph result texture, so we
    // can re-query that cache entry and keep the existing pipeline hot instead of recompiling it.
    auto tex_name = std::string(m_desc.result);
    if (scene.renderTargets.count(tex_name) == 0) {
        setPrepared(false);
        return;
    }
    auto& rt = scene.renderTargets.at(tex_name);
    if (auto opt = device.tex_cache().Query(tex_name, ToTexKey(rt), !rt.allowReuse);
        opt.has_value()) {
        m_desc.vk_result = opt.value();
    } else {
        setPrepared(false);
    }
}

void FinPass::execute(const Device& device, RenderingResources& rr) {
    auto& cmd    = rr.command;
    auto& outext = m_desc.vk_present.extent;

    // Surface swapchains are currently recreated with a new VulkanRender, but keep the final pass
    // correct if an output binding starts replacing present images in place. The previous frame is
    // fenced before this method runs, so the old framebuffer and multisample attachment can be
    // retired before allocating the new extent.
    m_desc.fb = {};
    if (! EnsureMultisampleColorImage(device,
                                      outext,
                                      m_desc.present_format,
                                      m_sample_count,
                                      m_msaa_color)) {
        setPrepared(false);
        return;
    }

    VkImageSubresourceRange base_srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_ARRAY_LAYERS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_MIP_LEVELS,

    };
    {
        const auto plan = BuildFinalOutputAttachmentPlan(m_sample_count);
        std::array<VkImageView, 2> attachments {
            plan.uses_resolve ? *m_msaa_color.view : m_desc.vk_present.view,
            m_desc.vk_present.view,
        };
        VkFramebufferCreateInfo info {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .renderPass = *m_desc.pipeline.pass,
            .attachmentCount = plan.attachment_count,
            .pAttachments = attachments.data(),
            .width = m_desc.vk_present.extent.width,
            .height = m_desc.vk_present.extent.height,
            .layers = 1,
        };
        if (device.handle().CreateFramebuffer(info, m_desc.fb) != VK_SUCCESS) return;
    }
    {
        VkDescriptorImageInfo desc_img {
            .sampler     = m_desc.vk_result.sampler,
            .imageView   = m_desc.vk_result.view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet wset {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext           = nullptr,
            .dstSet          = {},
            .dstBinding      = 1,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &desc_img,
        };
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wset);
    }

    // do queue family transfer operation
    if (m_desc.present_queue_index != device.graphics_queue().family_index) {
        VkImageMemoryBarrier imb {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout           = m_desc.present_layout,
            .newLayout           = m_desc.present_layout,
            .srcQueueFamilyIndex = m_desc.present_queue_index,
            .dstQueueFamilyIndex = device.graphics_queue().family_index,
            .image               = m_desc.vk_present.handle,
            .subresourceRange    = base_srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            imb);
    }
    VkRenderPassBeginInfo pass_begin_info {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext       = nullptr,
        .renderPass  = *m_desc.pipeline.pass,
        .framebuffer = *m_desc.fb,
        .renderArea =
            VkRect2D {
                .offset = { 0, 0 },
                .extent = { outext.width, outext.height },
            },
        .clearValueCount = 1,
        .pClearValues    = &m_desc.clear_value,
    };
    cmd.BeginRenderPass(pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.handle);
    VkViewport viewport {
        .x        = 0,
        .y        = (float)outext.height,
        .width    = (float)outext.width,
        .height   = -(float)outext.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor { { 0, 0 }, { outext.width, outext.height } };
    cmd.SetViewport(0, viewport);
    cmd.SetScissor(0, scissor);

    cmd.BindVertexBuffers(
        0, 1, std::array { rr.vertex_buf->gpuBuf() }.data(), &m_desc.vertex_buf.offset);
    cmd.Draw(4, 1, 0, 0);
    cmd.EndRenderPass();

    // do queue family transfer operation
    if (m_desc.present_queue_index != device.graphics_queue().family_index) {
        VkImageMemoryBarrier imb {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout           = m_desc.present_layout,
            .newLayout           = m_desc.present_layout,
            .srcQueueFamilyIndex = device.graphics_queue().family_index,
            .dstQueueFamilyIndex = m_desc.present_queue_index,
            .image               = m_desc.vk_present.handle,
            .subresourceRange    = base_srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            imb);
    }
}
void FinPass::destory(const Device&, RenderingResources& rr) {
    setPrepared(false);
    clearReleaseTexs();
    if (m_desc.vertex_buf) rr.vertex_buf->unallocateSubRef(m_desc.vertex_buf);
    m_desc.vertex_buf = {};
    m_msaa_color = {};
}
