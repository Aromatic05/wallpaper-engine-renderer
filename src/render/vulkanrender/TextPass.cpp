#include "TextPass.hpp"

#include "PassCommon.hpp"
#include "Resource.hpp"
#include "SpecTexs.hpp"
#include "interface/IShaderValueUpdater.h"
#include "scene/SceneShader.h"
#include "scene/SceneMesh.h"
#include "scene/Image.hpp"
#include "scene/SceneTextPrimitive.h"
#include "utils/Logging.h"
#include "vulkan/ShaderComp.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <array>
#include <cstdint>

using namespace wallpaper::vulkan;

namespace
{
struct TextPassUniforms {
    float model_view_projection[16] {};
    float color[4] {};
};

std::array<float, 4> ResolveTextColor(const wallpaper::SceneTextPrimitive& primitive,
                                      bool                                 background) {
    if (background) {
        return {
            primitive.object.backgroundcolor[0] * primitive.object.backgroundbrightness,
            primitive.object.backgroundcolor[1] * primitive.object.backgroundbrightness,
            primitive.object.backgroundcolor[2] * primitive.object.backgroundbrightness,
            primitive.object.alpha,
        };
    }
    return {
        primitive.object.color[0],
        primitive.object.color[1],
        primitive.object.color[2],
        primitive.object.alpha,
    };
}

void WriteMatrixToUniform(TextPassUniforms& uniforms, const Eigen::Matrix4f& matrix) {
    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) {
            uniforms.model_view_projection[column * 4 + row] = matrix(row, column);
        }
    }
}

std::shared_ptr<wallpaper::Image> ResolveTextBackgroundImage() {
    static const std::shared_ptr<wallpaper::Image> image = [] {
        auto result = std::make_shared<wallpaper::Image>();
        result->key = "__text_layer_background_white";
        result->revision = 1;
        result->header.width = 1;
        result->header.height = 1;
        result->header.mapWidth = 1;
        result->header.mapHeight = 1;
        result->header.count = 1;
        result->header.format = wallpaper::TextureFormat::RGBA8;
        result->header.type = wallpaper::ImageType::PNG;
        result->header.sample.wrapS = wallpaper::TextureWrap::CLAMP_TO_EDGE;
        result->header.sample.wrapT = wallpaper::TextureWrap::CLAMP_TO_EDGE;
        result->header.sample.magFilter = wallpaper::TextureFilter::LINEAR;
        result->header.sample.minFilter = wallpaper::TextureFilter::LINEAR;
        result->slots.resize(1);
        result->slots[0].width = 1;
        result->slots[0].height = 1;
        wallpaper::ImageData mipmap;
        mipmap.width = 1;
        mipmap.height = 1;
        mipmap.size = 4;
        auto pixels = std::make_unique<uint8_t[]>(4);
        pixels[0] = 255;
        pixels[1] = 255;
        pixels[2] = 255;
        pixels[3] = 255;
        mipmap.data = wallpaper::ImageDataPtr(pixels.release(), [](uint8_t* ptr) { delete[] ptr; });
        result->slots[0].mipmaps.push_back(std::move(mipmap));
        return result;
    }();
    return image;
}

bool LoadTextPassTexture(const Device& device,
                         const std::shared_ptr<wallpaper::Image>& image,
                         ImageSlotsRef* out_slots) {
    if (out_slots == nullptr) return false;
    if (image == nullptr) {
        *out_slots = {};
        return true;
    }
    *out_slots = device.tex_cache().CreateTex(*image);
    return !out_slots->slots.empty();
}

std::optional<vvk::RenderPass> CreateTextRenderPass(const vvk::Device& device,
                                                    VkFormat           format,
                                                    VkAttachmentLoadOp load_op) {
    VkAttachmentDescription attachment {
        .format         = format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = load_op,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = load_op == VK_ATTACHMENT_LOAD_OP_LOAD
                            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkAttachmentReference color_ref {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_ref,
    };
    VkSubpassDependency dependency {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo create_info {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &attachment,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dependency,
    };
    vvk::RenderPass pass;
    if (device.CreateRenderPass(create_info, pass) != VK_SUCCESS) return std::nullopt;
    return pass;
}

bool BuildTextPipeline(
    const Device&                        device,
    const wallpaper::SceneTextPrimitive& primitive,
    bool                                 offscreen_output,
    PipelineParameters&                  pipeline_parameters) {
    static const char* kVertexSource = R"(
#version 450
layout(binding = 0) uniform TextUniformBlock {
    mat4 g_ModelViewProjectionMatrix;
    vec4 g_Color4;
};
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out vec4 v_Color;
void main() {
    gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);
    v_TexCoord = a_TexCoord;
    v_Color = g_Color4;
}
)";

    static const char* kFragmentSource = R"(
#version 450
layout(binding = 1) uniform sampler2D g_Texture0;
layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in vec4 v_Color;
layout(location = 0) out vec4 outColor;
void main() {
    float coverage = texture(g_Texture0, v_TexCoord).a;
    outColor = vec4(v_Color.rgb, v_Color.a * coverage);
}
)";

    ShaderCompOpt opt {};
    opt.client_ver = glslang::EShTargetVulkan_1_0;

    std::array<ShaderCompUnit, 2> units {
        ShaderCompUnit { .stage = EShLangVertex, .src = std::string(kVertexSource) },
        ShaderCompUnit { .stage = EShLangFragment, .src = std::string(kFragmentSource) },
    };
    std::vector<Uni_ShaderSpv> spvs;
    if (!CompileAndLinkShaderUnits(units, opt, spvs)) return false;

    const wallpaper::SceneMesh* source_mesh { nullptr };
    if (primitive.background_mesh != nullptr && primitive.background_mesh->VertexCount() > 0) {
        source_mesh = primitive.background_mesh.get();
    } else {
        for (const auto& page : primitive.glyph_pages) {
            if (page.mesh != nullptr && page.mesh->VertexCount() > 0) {
                source_mesh = page.mesh.get();
                break;
            }
        }
    }
    if (source_mesh == nullptr) return false;

    const auto& vertex = source_mesh->GetVertexArray(0);
    const auto  attrs  = vertex.GetAttrOffsetMap();
    const auto  position_it = attrs.find(std::string(wallpaper::WE_IN_POSITION));
    const auto  texcoord_it = attrs.find(std::string(wallpaper::WE_IN_TEXCOORD));
    if (position_it == attrs.end() || texcoord_it == attrs.end()) return false;

    DescriptorSetInfo descriptor_info;
    descriptor_info.push_descriptor = true;
    descriptor_info.bindings = {
        VkDescriptorSetLayoutBinding {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        VkDescriptorSetLayoutBinding {
            .binding         = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkVertexInputBindingDescription binding {
        .binding   = 0,
        .stride    = static_cast<uint32_t>(vertex.OneSizeOf()),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    std::array<VkVertexInputAttributeDescription, 2> attributes {
        VkVertexInputAttributeDescription {
            .location = 0,
            .binding  = 0,
            .format   = VK_FORMAT_R32G32B32_SFLOAT,
            .offset   = static_cast<uint32_t>(position_it->second.offset),
        },
        VkVertexInputAttributeDescription {
            .location = 1,
            .binding  = 0,
            .format   = VK_FORMAT_R32G32_SFLOAT,
            .offset   = static_cast<uint32_t>(texcoord_it->second.offset),
        },
    };

    VkPipelineColorBlendAttachmentState blend_state {};
    blend_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    SetBlend(wallpaper::BlendMode::Translucent, blend_state);

    auto render_pass =
        CreateTextRenderPass(device.handle(),
                             VK_FORMAT_R8G8B8A8_UNORM,
                             offscreen_output ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                              : VK_ATTACHMENT_LOAD_OP_LOAD);
    if (!render_pass.has_value()) return false;

    GraphicsPipeline pipeline;
    pipeline.toDefault();
    pipeline.addDescriptorSetInfo(std::span<const DescriptorSetInfo>(&descriptor_info, 1))
        .setColorBlendStates(std::span<const VkPipelineColorBlendAttachmentState>(&blend_state, 1))
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .addInputBindingDescription(std::span<const VkVertexInputBindingDescription>(&binding, 1))
        .addInputAttributeDescription(attributes);
    for (auto& spv : spvs) {
        pipeline.addStage(std::move(spv));
    }
    return pipeline.create(device, *render_pass, pipeline_parameters);
}
} // namespace

TextPass::TextPass(const Desc& desc) {
    m_desc.scene     = desc.scene;
    m_desc.node      = desc.node;
    m_desc.layer_id  = desc.layer_id;
    m_desc.output    = desc.output;
    m_desc.clear_output = desc.clear_output;
}

TextPass::~TextPass() = default;

std::string TextPass::residencyKey() const {
    return "TextPass|node=" + std::to_string(reinterpret_cast<std::uintptr_t>(m_desc.node)) +
           "|layer=" + std::to_string(m_desc.layer_id) + "|output=" + m_desc.output;
}

bool TextPass::canReuseForResidency(const VulkanPass& next_pass) const {
    const auto* next = dynamic_cast<const TextPass*>(&next_pass);
    return next != nullptr && residencyKey() == next->residencyKey() &&
           m_desc.clear_output == next->m_desc.clear_output;
}

void TextPass::absorbResidencyGraphState(const VulkanPass& next_pass) {
    const auto* next = dynamic_cast<const TextPass*>(&next_pass);
    if (next == nullptr) return;
    m_desc.scene        = next->m_desc.scene;
    m_desc.node         = next->m_desc.node;
    m_desc.layer_id     = next->m_desc.layer_id;
    m_desc.output       = next->m_desc.output;
    m_desc.clear_output = next->m_desc.clear_output;
}

bool TextPass::referencesRenderTarget(std::string_view render_target) const {
    return m_desc.output == render_target;
}

bool TextPass::referencesTextLayer(int32_t layer_id) const {
    return layer_id != 0 && m_desc.layer_id == layer_id;
}

wallpaper::SceneTextPrimitive* TextPass::primitive() const {
    if (m_desc.scene == nullptr) return nullptr;
    auto it = m_desc.scene->textPrimitives.find(m_desc.layer_id);
    if (it == m_desc.scene->textPrimitives.end()) return nullptr;
    return it->second.get();
}

bool TextPass::refreshTextures(const Device& device) {
    auto* primitive = this->primitive();
    if (primitive == nullptr) return false;

    if (!LoadTextPassTexture(device, ResolveTextBackgroundImage(), &m_desc.background_texture)) {
        return false;
    }
    if (primitive->layout.glyph_pages.size() != primitive->glyph_pages.size()) return false;
    m_desc.page_textures.resize(primitive->layout.glyph_pages.size());
    for (size_t i = 0; i < primitive->layout.glyph_pages.size(); i++) {
        const auto& atlas_page = primitive->layout.glyph_pages[i];
        if (!LoadTextPassTexture(device, atlas_page.image, &m_desc.page_textures[i])) {
            return false;
        }
    }
    m_loaded_atlas_version = primitive->atlas_version;
    return true;
}

bool TextPass::recreateFramebuffer(const Device& device) {
    m_desc.framebuffer.reset();
    if (!m_desc.pipeline.pass || m_desc.vk_output.view == VK_NULL_HANDLE ||
        m_desc.vk_output.extent.width == 0 || m_desc.vk_output.extent.height == 0) {
        return false;
    }
    VkFramebufferCreateInfo info {
        .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass      = *m_desc.pipeline.pass,
        .attachmentCount = 1,
        .pAttachments    = &m_desc.vk_output.view,
        .width           = m_desc.vk_output.extent.width,
        .height          = m_desc.vk_output.extent.height,
        .layers          = 1,
    };
    return device.handle().CreateFramebuffer(info, m_desc.framebuffer) == VK_SUCCESS;
}

bool TextPass::ensureMeshBuffers(SceneMesh& mesh, MeshBuffers& buffers, RenderingResources& rr) {
    auto* dyn_buf = rr.dyn_buf;
    if (dyn_buf == nullptr) return false;

    while (buffers.vertex_bufs.size() > mesh.VertexCount()) {
        dyn_buf->unallocateSubRef(buffers.vertex_bufs.back());
        buffers.vertex_bufs.pop_back();
    }
    buffers.vertex_bufs.resize(mesh.VertexCount());

    for (usize array_index = 0; array_index < mesh.VertexCount(); array_index++) {
        const auto& vertex = mesh.GetVertexArray(array_index);
        auto& subref = buffers.vertex_bufs[array_index];
        const auto required_size =
            static_cast<VkDeviceSize>(std::max<usize>(vertex.CapacitySizeOf(), vertex.OneSizeOf()));
        if (!subref || subref.size < required_size) {
            if (subref) dyn_buf->unallocateSubRef(subref);
            if (!dyn_buf->allocateSubRef(required_size, subref)) return false;
            buffers.force_upload = true;
        }
    }

    if (mesh.IndexCount() > 0) {
        const auto& index = mesh.GetIndexArray(0);
        const auto required_size =
            static_cast<VkDeviceSize>(std::max<usize>(index.CapacitySizeof(), sizeof(uint16_t) * 6));
        if (!buffers.index_buf || buffers.index_buf.size < required_size) {
            if (buffers.index_buf) dyn_buf->unallocateSubRef(buffers.index_buf);
            if (!dyn_buf->allocateSubRef(required_size, buffers.index_buf)) return false;
            buffers.force_upload = true;
        }
    }

    const bool needs_upload = mesh.Dirty().load() || buffers.force_upload;
    if (!needs_upload) return true;

    for (usize array_index = 0; array_index < mesh.VertexCount(); array_index++) {
        const auto& vertex = mesh.GetVertexArray(array_index);
        auto& subref = buffers.vertex_bufs[array_index];
        if (!dyn_buf->writeToBuf(subref,
                                 { reinterpret_cast<uint8_t*>(const_cast<float*>(vertex.Data())),
                                   vertex.DataSizeOf() })) {
            return false;
        }
    }

    if (mesh.IndexCount() > 0) {
        const auto& index = mesh.GetIndexArray(0);
        buffers.draw_count = static_cast<uint32_t>((index.RenderDataCount() * 2) / 3) * 3;
        if (!dyn_buf->writeToBuf(buffers.index_buf,
                                 { reinterpret_cast<uint8_t*>(const_cast<uint32_t*>(index.Data())),
                                   index.DataSizeOf() })) {
            return false;
        }
    } else {
        buffers.draw_count = mesh.VertexCount() > 0
            ? static_cast<uint32_t>(mesh.GetVertexArray(0).VertexCount())
            : 0;
    }

    mesh.Dirty().store(false);
    buffers.force_upload = false;
    return true;
}

void TextPass::prepare(Scene& scene, const Device& device, RenderingResources& rr) {
    m_desc.scene = &scene;
    auto* primitive = this->primitive();
    if (primitive == nullptr || m_desc.node == nullptr || m_desc.output.empty()) return;

    auto output_it = scene.renderTargets.find(m_desc.output);
    if (output_it == scene.renderTargets.end()) return;
    auto output = device.tex_cache().Query(m_desc.output, ToTexKey(output_it->second), !output_it->second.allowReuse);
    if (!output.has_value()) return;
    m_desc.vk_output = output.value();

    if (!refreshTextures(device)) return;

    const bool offscreen_output = m_desc.output != SpecTex_Default;
    if (!BuildTextPipeline(device, *primitive, offscreen_output, m_desc.pipeline)) return;

    if (!recreateFramebuffer(device)) return;
    if (rr.dyn_buf == nullptr) return;
    rr.dyn_buf->allocateSubRef(sizeof(TextPassUniforms),
                               m_desc.ubo_buf,
                               device.limits().minUniformBufferOffsetAlignment);

    if (primitive->background_mesh != nullptr) {
        m_background_buffers.force_upload = true;
        if (!ensureMeshBuffers(*primitive->background_mesh, m_background_buffers, rr)) return;
    }
    m_page_buffers.resize(primitive->glyph_pages.size());
    for (size_t page_index = 0; page_index < primitive->glyph_pages.size(); page_index++) {
        m_page_buffers[page_index].force_upload = true;
        if (primitive->glyph_pages[page_index].mesh != nullptr &&
            !ensureMeshBuffers(*primitive->glyph_pages[page_index].mesh, m_page_buffers[page_index], rr)) {
            return;
        }
    }

    m_desc.clear_value = VkClearValue {
        .color = {
            offscreen_output ? 0.0f : scene.clearColor[0],
            offscreen_output ? 0.0f : scene.clearColor[1],
            offscreen_output ? 0.0f : scene.clearColor[2],
            offscreen_output ? 0.0f : 1.0f,
        },
    };
    setPrepared();
}

void TextPass::refreshResources(Scene& scene, const Device& device, RenderingResources& rr) {
    (void)scene;
    auto* primitive = this->primitive();
    if (primitive == nullptr) {
        setPrepared(false);
        return;
    }
    if (primitive->atlas_version != m_loaded_atlas_version ||
        m_desc.page_textures.size() != primitive->glyph_pages.size()) {
        if (!refreshTextures(device)) {
            setPrepared(false);
            return;
        }
    }
    if (primitive->background_mesh != nullptr &&
        !ensureMeshBuffers(*primitive->background_mesh, m_background_buffers, rr)) {
        setPrepared(false);
        return;
    }
    if (m_page_buffers.size() != primitive->glyph_pages.size()) {
        m_page_buffers.resize(primitive->glyph_pages.size());
        for (auto& buffers : m_page_buffers) buffers.force_upload = true;
    }
    for (size_t page_index = 0; page_index < primitive->glyph_pages.size(); page_index++) {
        if (primitive->glyph_pages[page_index].mesh != nullptr &&
            !ensureMeshBuffers(*primitive->glyph_pages[page_index].mesh, m_page_buffers[page_index], rr)) {
            setPrepared(false);
            return;
        }
    }
    auto output_it = scene.renderTargets.find(m_desc.output);
    if (output_it == scene.renderTargets.end()) return;
    auto output = device.tex_cache().Query(m_desc.output, ToTexKey(output_it->second), !output_it->second.allowReuse);
    if (output.has_value()) m_desc.vk_output = output.value();
    if (!m_desc.framebuffer || m_desc.vk_output.view == VK_NULL_HANDLE) {
        if (!recreateFramebuffer(device)) {
            setPrepared(false);
            return;
        }
    }
}

bool TextPass::warmupPipeline(Scene& scene, const Device& device, RenderingResources& rr) {
    (void)scene;
    (void)rr;
    auto* primitive = this->primitive();
    if (primitive == nullptr || m_desc.node == nullptr) return false;
    const bool offscreen_output = m_desc.output != SpecTex_Default;
    return BuildTextPipeline(device, *primitive, offscreen_output, m_desc.pipeline);
}

void TextPass::execute(const Device&, RenderingResources& rr) {
    auto* primitive = this->primitive();
    if (primitive == nullptr || !m_desc.pipeline.handle || !m_desc.pipeline.pass || !m_desc.framebuffer) {
        setPrepared(false);
        return;
    }

    if (primitive->atlas_version != m_loaded_atlas_version ||
        m_desc.page_textures.size() != primitive->glyph_pages.size()) {
        setPrepared(false);
        return;
    }

    auto update_uniforms = [&](const std::array<float, 4>& color) {
        TextPassUniforms uniforms {};
        Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
        if (m_desc.scene != nullptr && m_desc.scene->shaderValueUpdater != nullptr && m_desc.node != nullptr) {
            sprite_map_t sprites;
            m_desc.scene->shaderValueUpdater->UpdateUniforms(
                m_desc.node,
                sprites,
                [&](std::string_view name, wallpaper::ShaderValue value) {
                    if (name != wallpaper::G_MVP || value.size() < 16) return;
                    for (int column = 0; column < 4; column++) {
                        for (int row = 0; row < 4; row++) {
                            matrix(row, column) = value[static_cast<size_t>(column * 4 + row)];
                        }
                    }
                });
        }
        WriteMatrixToUniform(uniforms, matrix);
        std::copy(color.begin(), color.end(), uniforms.color);
        rr.dyn_buf->writeToBuf(m_desc.ubo_buf,
                               { reinterpret_cast<uint8_t*>(&uniforms), sizeof(uniforms) });
    };

    auto bind_uniforms = [&]() {
        VkDescriptorBufferInfo buffer_info {
            rr.dyn_buf->gpuBuf(),
            m_desc.ubo_buf.offset,
            m_desc.ubo_buf.size,
        };
        VkWriteDescriptorSet write {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &buffer_info,
        };
        rr.command.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, write);
    };

    auto bind_texture = [&](const ImageSlotsRef& slots) {
        if (slots.slots.empty()) return;
        const auto& image = slots.getActive();
        VkDescriptorImageInfo image_info {
            image.sampler,
            image.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet write {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding      = 1,
            .descriptorCount = 1,
            .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &image_info,
        };
        rr.command.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, write);
    };

    const VkExtent2D output_extent {
        .width  = m_desc.vk_output.extent.width,
        .height = m_desc.vk_output.extent.height,
    };
    VkRenderPassBeginInfo begin_info {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass      = *m_desc.pipeline.pass,
        .framebuffer     = *m_desc.framebuffer,
        .renderArea      = VkRect2D { .offset = { 0, 0 }, .extent = output_extent },
        .clearValueCount = 1,
        .pClearValues    = &m_desc.clear_value,
    };
    rr.command.BeginRenderPass(begin_info, VK_SUBPASS_CONTENTS_INLINE);
    rr.command.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.handle);
    std::array<VkViewport, 1> viewport {
        VkViewport {
            .x = 0.0f,
            .y = static_cast<float>(m_desc.vk_output.extent.height),
            .width = static_cast<float>(m_desc.vk_output.extent.width),
            .height = -static_cast<float>(m_desc.vk_output.extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        }
    };
    std::array<VkRect2D, 1> scissor { VkRect2D { { 0, 0 }, output_extent } };
    rr.command.SetViewport(0, viewport);
    rr.command.SetScissor(0, scissor);

    auto draw_mesh = [&](MeshBuffers& buffers,
                         const ImageSlotsRef& texture,
                         const std::array<float, 4>& color) {
        if (buffers.draw_count == 0) return;
        update_uniforms(color);
        bind_uniforms();
        bind_texture(texture);
        auto gpu_buf = rr.dyn_buf->gpuBuf();
        for (usize binding_index = 0; binding_index < buffers.vertex_bufs.size(); binding_index++) {
            auto& subref = buffers.vertex_bufs[binding_index];
            rr.command.BindVertexBuffers(static_cast<uint32_t>(binding_index), 1, &gpu_buf, &subref.offset);
        }
        if (buffers.index_buf) {
            rr.command.BindIndexBuffer(gpu_buf, buffers.index_buf.offset, VK_INDEX_TYPE_UINT16);
            rr.command.DrawIndexed(buffers.draw_count, 1, 0, 0, 0);
        } else {
            rr.command.Draw(buffers.draw_count, 1, 0, 0);
        }
    };

    if (primitive->object.opaquebackground && primitive->background_mesh != nullptr) {
        draw_mesh(m_background_buffers, m_desc.background_texture, ResolveTextColor(*primitive, true));
    }
    for (size_t page_index = 0; page_index < primitive->glyph_pages.size(); page_index++) {
        if (page_index >= m_desc.page_textures.size()) break;
        if (primitive->glyph_pages[page_index].mesh == nullptr) continue;
        draw_mesh(m_page_buffers[page_index],
                  m_desc.page_textures[page_index],
                  ResolveTextColor(*primitive, false));
    }
    rr.command.EndRenderPass();
}

void TextPass::destory(const Device&, RenderingResources& rr) {
    m_desc.framebuffer.reset();
    m_desc.vk_output = {};
    m_desc.background_texture = {};
    m_desc.page_textures.clear();
    if (rr.dyn_buf != nullptr) {
        for (auto& subref : m_background_buffers.vertex_bufs) rr.dyn_buf->unallocateSubRef(subref);
        if (m_background_buffers.index_buf) rr.dyn_buf->unallocateSubRef(m_background_buffers.index_buf);
        for (auto& page_buffers : m_page_buffers) {
            for (auto& subref : page_buffers.vertex_bufs) rr.dyn_buf->unallocateSubRef(subref);
            if (page_buffers.index_buf) rr.dyn_buf->unallocateSubRef(page_buffers.index_buf);
        }
        rr.dyn_buf->unallocateSubRef(m_desc.ubo_buf);
    }
    m_background_buffers = {};
    m_page_buffers.clear();
    m_desc.ubo_buf = {};
    setPrepared(false);
}
