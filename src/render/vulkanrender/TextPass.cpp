#include "TextPass.hpp"

#include "scene/Scene.h"
#include "scene/SceneNode.h"
#include "scene/SceneTextPrimitive.h"
#include "SpecTexs.hpp"
#include "utils/Logging.h"
#include "vulkan/ShaderComp.hpp"
#include "PassCommon.hpp"
#include "Resource.hpp"
#include "WPSceneScriptMedia.hpp"

#include <Eigen/Dense>
#include <cstdint>

using namespace wallpaper::vulkan;

namespace
{
constexpr std::string_view kTextBackgroundTextureKey { "__text_layer_background_white" };

std::string TextPipelineCompatibilityKey(bool offscreen_output) {
    // Text PSOs are shared by render-pass compatibility plus the full GraphicsPipeline descriptor,
    // not by the layer that first requested them. This keeps visibility toggles on the same model
    // as engine-level PSO caches while still letting hidden text release atlas/framebuffer memory.
    return "TextPass|format=rgba8|final=shader-read|load=" +
           std::to_string(static_cast<int>(offscreen_output ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                            : VK_ATTACHMENT_LOAD_OP_LOAD));
}

struct TextPassUniforms {
    float model_view_projection[16] {};
    float color[4] {};
};

struct PreparedTextShaders {
    std::vector<Uni_ShaderSpv> stages;
};

struct TextVertexInputLayout {
    VkVertexInputBindingDescription binding {};
    std::array<VkVertexInputAttributeDescription, 2> attributes {};
};

std::optional<TextVertexInputLayout> ResolveTextVertexInputLayout(
    const wallpaper::SceneTextPrimitive& primitive) {
    const wallpaper::SceneMesh* source_mesh { nullptr };
    for (const auto& page : primitive.glyph_pages) {
        if (page.mesh != nullptr && page.mesh->VertexCount() > 0) {
            source_mesh = page.mesh.get();
            break;
        }
    }
    if (source_mesh == nullptr && primitive.background_mesh != nullptr &&
        primitive.background_mesh->VertexCount() > 0) {
        source_mesh = primitive.background_mesh.get();
    }
    if (source_mesh == nullptr || source_mesh->VertexCount() == 0) return std::nullopt;

    const auto& vertex = source_mesh->GetVertexArray(0);
    const auto  attrs = vertex.GetAttrOffsetMap();
    const auto  position_it = attrs.find(std::string(wallpaper::WE_IN_POSITION));
    const auto  texcoord_it = attrs.find(std::string(wallpaper::WE_IN_TEXCOORD));
    if (position_it == attrs.end() || texcoord_it == attrs.end()) {
        LOG_ERROR("TextPass: generated text mesh is missing required position/texcoord attributes");
        return std::nullopt;
    }

    TextVertexInputLayout layout;
    // SceneVertexArray pads each attribute to Wallpaper Engine's vec4-style storage contract.
    // TextPass used to hardcode FLOAT3+FLOAT2 as a tightly packed 5-float vertex, but the actual
    // generated buffer is 8 floats per vertex. Reading the live SceneVertexArray stride/offsets
    // here keeps the dedicated text primitive on the same canonical mesh layout as generic image
    // passes and prevents every vertex after the first one from being fetched at the wrong byte.
    layout.binding = VkVertexInputBindingDescription {
        .binding = 0,
        .stride = static_cast<uint32_t>(vertex.OneSizeOf()),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    layout.attributes = {
        VkVertexInputAttributeDescription {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = static_cast<uint32_t>(position_it->second.offset),
        },
        VkVertexInputAttributeDescription {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = static_cast<uint32_t>(texcoord_it->second.offset),
        },
    };
    return layout;
}

std::optional<PreparedTextShaders> CompileTextShaders() {
    static const char* kVertexSource = R"(
[[vk::binding(0, 0)]] cbuffer TextUniformBlock {
    column_major float4x4 g_ModelViewProjectionMatrix;
    float4 g_Color4;
};

struct VSInput {
    [[vk::location(0)]] float3 a_Position : A_POSITION;
    [[vk::location(1)]] float2 a_TexCoord : A_TEXCOORD;
};

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 v_TexCoord : TEXCOORD0;
    [[vk::location(1)]] float4 v_Color : COLOR0;
};

VSOutput main_vs(VSInput input) {
    VSOutput output;
    output.position = mul(g_ModelViewProjectionMatrix, float4(input.a_Position, 1.0));
    output.v_TexCoord = input.a_TexCoord;
    output.v_Color = g_Color4;
    return output;
}
)";

    static const char* kFragmentSource = R"(
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float4> g_Texture0;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState g_Texture0_ww_sampler;

struct PSInput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 v_TexCoord : TEXCOORD0;
    [[vk::location(1)]] float4 v_Color : COLOR0;
};

float4 main_ps(PSInput input) : SV_Target0 {
    const float coverage = g_Texture0.Sample(g_Texture0_ww_sampler, input.v_TexCoord).a;
    return float4(input.v_Color.rgb, input.v_Color.a * coverage);
}
)";

    ShaderCompOpt options {};
    options.target_env = ShaderTargetEnv::VULKAN_1_0;
    options.auto_map_locations = false;
    options.auto_map_bindings = false;

    std::array<ShaderCompUnit, 2> units {
        ShaderCompUnit {
            .stage = wallpaper::ShaderType::VERTEX,
            .source_language = ShaderSourceLanguage::HLSL,
            .debug_name = "TextPass.vert",
            .entry_point = "main_vs",
            .src = kVertexSource,
        },
        ShaderCompUnit {
            .stage = wallpaper::ShaderType::FRAGMENT,
            .source_language = ShaderSourceLanguage::HLSL,
            .debug_name = "TextPass.frag",
            .entry_point = "main_ps",
            .src = kFragmentSource,
        },
    };

    PreparedTextShaders prepared;
    if (!CompileAndLinkShaderUnits(units, options, prepared.stages)) {
        LOG_ERROR("TextPass: failed to compile dedicated text shaders");
        return std::nullopt;
    }
    return prepared;
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
        .srcAccessMask = {},
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

void WriteMatrixToUniform(TextPassUniforms& uniforms, const Eigen::Matrix4f& matrix) {
    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) {
            uniforms.model_view_projection[column * 4 + row] = matrix(row, column);
        }
    }
}

std::shared_ptr<wallpaper::Image> ResolveTextBackgroundImage() {
    // The direct text pipeline only needs one non-glyph texture: a 1x1 white coverage image for
    // the optional opaque background quad. Materializing it here keeps the text pass self-owned
    // and avoids routing primitive text rendering through unrelated image-parser infrastructure.
    static const std::shared_ptr<wallpaper::Image> image =
        wallpaper::CreateSceneScriptSolidImage(kTextBackgroundTextureKey, { 255, 255, 255, 255 });
    return image;
}

bool LoadTextPassTexture(const Device&                        device,
                         const std::shared_ptr<wallpaper::Image>& image,
                         ImageSlotsRef*                       out_slots) {
    if (out_slots == nullptr) return false;
    if (image == nullptr) {
        *out_slots = {};
        return true;
    }

    *out_slots = device.tex_cache().CreateTex(*image);
    return !out_slots->slots.empty();
}

bool CreateTextPipelineForPrimitive(const Device&                         device,
                                    RenderingResources&                   rr,
                                    const wallpaper::SceneTextPrimitive&  primitive,
                                    bool                                  offscreen_output,
                                    std::string                           debug_name,
                                    PipelineParameters&                   pipeline_parameters) {
    auto render_pass =
        CreateTextRenderPass(device.handle(),
                             VK_FORMAT_R8G8B8A8_UNORM,
                             offscreen_output ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
    if (!render_pass.has_value()) return false;

    const auto compiled_shaders = CompileTextShaders();
    if (!compiled_shaders.has_value()) return false;

    DescriptorSetInfo descriptor_info;
    descriptor_info.push_descriptor = true;
    descriptor_info.bindings = {
        VkDescriptorSetLayoutBinding {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        VkDescriptorSetLayoutBinding {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    const auto vertex_layout = ResolveTextVertexInputLayout(primitive);
    if (!vertex_layout.has_value()) return false;

    VkPipelineColorBlendAttachmentState blend_state {};
    blend_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    SetBlend(wallpaper::BlendMode::Translucent, blend_state);

    GraphicsPipeline pipeline;
    pipeline.toDefault();
    pipeline_parameters.debug_name = std::move(debug_name);
    pipeline_parameters.cache_key  = TextPipelineCompatibilityKey(offscreen_output);
    pipeline.addDescriptorSetInfo(std::span<const DescriptorSetInfo>(&descriptor_info, 1))
        .setColorBlendStates(std::span<const VkPipelineColorBlendAttachmentState>(&blend_state, 1))
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .addInputBindingDescription(
            std::span<const VkVertexInputBindingDescription>(&vertex_layout->binding, 1))
        .addInputAttributeDescription(vertex_layout->attributes);
    for (const auto& stage : compiled_shaders->stages) {
        if (!stage) continue;
        pipeline.addStage(Uni_ShaderSpv(new ShaderSpv(*stage)));
    }

    return pipeline.create(device, *render_pass, pipeline_parameters, rr.pipeline_cache.get());
}

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
} // namespace

TextPass::TextPass(const Desc& desc) {
    // The pass description intentionally stores only the authored/runtime identity fields here.
    // Vulkan handles such as framebuffers and pipeline objects are non-copyable and must always be
    // created during `prepare()` against the live device, so the constructor avoids copying any of
    // the prepared-state members from the temporary render-graph description.
    m_desc.scene               = desc.scene;
    m_desc.node                = desc.node;
    m_desc.layer_id            = desc.layer_id;
    m_desc.execute_when_hidden = desc.execute_when_hidden;
    m_desc.output              = desc.output;
}
TextPass::~TextPass() = default;

std::string TextPass::residencyKey() const {
    return "TextPass|node=" + std::to_string(reinterpret_cast<std::uintptr_t>(m_desc.node)) +
           "|layer=" + std::to_string(m_desc.layer_id) + "|output=" + m_desc.output;
}

bool TextPass::canReuseForResidency(const VulkanPass& next_pass) const {
    const auto* next = dynamic_cast<const TextPass*>(&next_pass);
    if (next == nullptr) return false;
    // The text pipeline depends on the text primitive's vertex layout and output target, both
    // represented by the stable node/layer/output residency key. Visibility gates and the live
    // scene pointer are safe to absorb without recreating shader modules or descriptor layouts.
    return residencyKey() == next->residencyKey() &&
           m_desc.execute_when_hidden == next->m_desc.execute_when_hidden;
}

void TextPass::absorbResidencyGraphState(const VulkanPass& next_pass) {
    const auto* next = dynamic_cast<const TextPass*>(&next_pass);
    if (next == nullptr) return;
    m_desc.scene               = next->m_desc.scene;
    m_desc.node                = next->m_desc.node;
    m_desc.layer_id            = next->m_desc.layer_id;
    m_desc.execute_when_hidden = next->m_desc.execute_when_hidden;
    m_desc.output              = next->m_desc.output;
}

bool TextPass::referencesRenderTarget(std::string_view render_target) const {
    // A text pass only owns its bridge output. Glyph atlas pages are imported texture-cache entries,
    // not render-graph targets, so they must not make unrelated text passes participate in a
    // render-target resize refresh.
    return m_desc.output == render_target;
}

bool TextPass::referencesTextLayer(int32_t layer_id) const {
    // Runtime text rerasters are scoped by authored layer id. Matching that id here lets a direct
    // Clock-style text pass refresh its atlas and mesh before command recording without touching
    // unrelated text layers that happen to draw to the same final render target.
    return layer_id != 0 && m_desc.layer_id == layer_id;
}

bool TextPass::refreshTextures(const Device& device) {
    const auto* primitive =
        m_desc.node != nullptr ? m_desc.node->Text() : nullptr;
    if (primitive == nullptr) return false;

    if (!LoadTextPassTexture(device, ResolveTextBackgroundImage(), &m_desc.background_texture)) {
        return false;
    }

    m_desc.page_textures.resize(primitive->glyph_pages.size());
    for (size_t page_index = 0; page_index < primitive->glyph_pages.size(); page_index++) {
        if (!LoadTextPassTexture(device,
                                 primitive->layout.glyph_pages[page_index].image,
                                 &m_desc.page_textures[page_index])) {
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
        LOG_ERROR("TextPassRefresh: cannot recreate framebuffer node='%s' output='%s' "
                  "hasRenderPass=%s hasView=%s extent=[%u,%u]",
                  m_desc.node != nullptr ? m_desc.node->Name().c_str() : "<null>",
                  m_desc.output.c_str(),
                  m_desc.pipeline.pass ? "true" : "false",
                  m_desc.vk_output.view != VK_NULL_HANDLE ? "true" : "false",
                  m_desc.vk_output.extent.width,
                  m_desc.vk_output.extent.height);
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
        auto&       subref = buffers.vertex_bufs[array_index];
        const auto  required_size =
            static_cast<VkDeviceSize>(std::max<usize>(vertex.CapacitySizeOf(), vertex.OneSizeOf()));
        if (!subref || subref.size < required_size) {
            if (subref) dyn_buf->unallocateSubRef(subref);
            if (!dyn_buf->allocateSubRef(required_size, subref)) return false;
            buffers.force_upload = true;
        }
    }

    if (mesh.IndexCount() > 0) {
        const auto& index = mesh.GetIndexArray(0);
        const auto  required_size =
            static_cast<VkDeviceSize>(std::max<usize>(index.CapacitySizeof(), sizeof(uint16_t) * 6));
        if (!buffers.index_buf || buffers.index_buf.size < required_size) {
            if (buffers.index_buf) dyn_buf->unallocateSubRef(buffers.index_buf);
            if (!dyn_buf->allocateSubRef(required_size, buffers.index_buf)) return false;
            buffers.force_upload = true;
        }
    } else if (buffers.index_buf) {
        dyn_buf->unallocateSubRef(buffers.index_buf);
        buffers.index_buf = {};
    }

    const bool needs_upload = mesh.Dirty().load() || buffers.force_upload;
    if (!needs_upload) return true;

    for (usize array_index = 0; array_index < mesh.VertexCount(); array_index++) {
        const auto& vertex = mesh.GetVertexArray(array_index);
        auto&       subref = buffers.vertex_bufs[array_index];
        if (!dyn_buf->writeToBuf(
                subref,
                { reinterpret_cast<uint8_t*>(const_cast<float*>(vertex.Data())), vertex.DataSizeOf() })) {
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
    const auto* primitive =
        m_desc.node != nullptr ? m_desc.node->Text() : nullptr;
    if (primitive == nullptr) return;

    if (!refreshTextures(device)) return;

    auto output_it = scene.renderTargets.find(m_desc.output);
    if (output_it == scene.renderTargets.end()) return;
    // Text bridge render targets can resize while the TextPass object is intentionally kept alive.
    // The existing framebuffer references the old TextureCache image view, so it must be released
    // before `Query()` is allowed to replace the backing image for this output.
    m_desc.framebuffer.reset();
    auto output = device.tex_cache().Query(m_desc.output,
                                           ToTexKey(output_it->second),
                                           !output_it->second.allowReuse);
    if (!output.has_value()) return;
    m_desc.vk_output = output.value();

    const bool offscreen_output = m_desc.output != wallpaper::SpecTex_Default;
    m_desc.clear_output = offscreen_output;
    const auto debug_name =
        "TextPass[node=" + (m_desc.node != nullptr ? m_desc.node->Name() : std::string("(null)")) +
        ",output=" + m_desc.output + "]";
    if (!CreateTextPipelineForPrimitive(
            device, rr, *primitive, offscreen_output, debug_name, m_desc.pipeline)) {
        return;
    }
    if (!recreateFramebuffer(device)) return;

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
        if (!ensureMeshBuffers(*primitive->glyph_pages[page_index].mesh,
                               m_page_buffers[page_index],
                               rr)) {
            return;
        }
    }

    // The dedicated text pass only requests the shared transform uniform contract. All visual text
    // state such as glyph color and background color comes directly from the text primitive, so no
    // generic image-material bootstrap is involved anymore.
    if (scene.shaderValueUpdater != nullptr && m_desc.node != nullptr) {
        scene.shaderValueUpdater->InitUniforms(
            m_desc.node,
            [](std::string_view uniform_name) {
                return uniform_name == wallpaper::G_MVP;
            });
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

bool TextPass::warmupPipeline(Scene& scene, const Device& device, RenderingResources& rr) {
    (void)scene;
    const auto* primitive =
        m_desc.node != nullptr ? m_desc.node->Text() : nullptr;
    if (primitive == nullptr) return false;

    const bool offscreen_output = m_desc.output != wallpaper::SpecTex_Default;
    const auto debug_name =
        "TextPassWarmup[node=" +
        (m_desc.node != nullptr ? m_desc.node->Name() : std::string("(null)")) +
        ",output=" + m_desc.output + "]";
    return CreateTextPipelineForPrimitive(
        device, rr, *primitive, offscreen_output, debug_name, m_desc.pipeline);
}

void TextPass::refreshResources(Scene& scene, const Device& device, RenderingResources& rr) {
    (void)scene;
    if (!refreshTextures(device)) {
        LOG_ERROR("TextPassRefresh: texture refresh failed node='%s' output='%s'",
                  m_desc.node != nullptr ? m_desc.node->Name().c_str() : "<null>",
                  m_desc.output.c_str());
        setPrepared(false);
        return;
    }
    auto* primitive = m_desc.node != nullptr ? m_desc.node->Text() : nullptr;
    if (primitive == nullptr) {
        LOG_ERROR("TextPassRefresh: missing primitive node='%s' output='%s'",
                  m_desc.node != nullptr ? m_desc.node->Name().c_str() : "<null>",
                  m_desc.output.c_str());
        setPrepared(false);
        return;
    }

    // Resource refresh happens before the next draw command records its dynamic-buffer upload.
    // Rebuilding and writing text meshes here keeps resized bridge text from binding freshly
    // allocated subranges that have not been copied to the GPU yet, which was the reason
    // effect-backed Date/Clock/Day could disappear immediately after a layout update.
    if (primitive->background_mesh != nullptr &&
        !ensureMeshBuffers(*primitive->background_mesh, m_background_buffers, rr)) {
        LOG_ERROR("TextPassRefresh: background mesh upload failed node='%s' output='%s'",
                  m_desc.node != nullptr ? m_desc.node->Name().c_str() : "<null>",
                  m_desc.output.c_str());
        setPrepared(false);
        return;
    }
    if (m_page_buffers.size() != primitive->glyph_pages.size()) {
        m_page_buffers.resize(primitive->glyph_pages.size());
        for (auto& buffers : m_page_buffers) buffers.force_upload = true;
    }
    for (size_t page_index = 0; page_index < primitive->glyph_pages.size(); page_index++) {
        if (!ensureMeshBuffers(*primitive->glyph_pages[page_index].mesh,
                               m_page_buffers[page_index],
                               rr)) {
            LOG_ERROR("TextPassRefresh: glyph mesh upload failed node='%s' output='%s' page=%zu",
                      m_desc.node != nullptr ? m_desc.node->Name().c_str() : "<null>",
                      m_desc.output.c_str(),
                      page_index);
            setPrepared(false);
            return;
        }
    }
    auto output_it = scene.renderTargets.find(m_desc.output);
    if (output_it == scene.renderTargets.end()) {
        setPrepared(false);
        return;
    }
    const auto previous_output_view = m_desc.vk_output.view;
    const auto previous_output_extent = m_desc.vk_output.extent;
    const auto desired_output_key = ToTexKey(output_it->second);
    const bool output_extent_changed =
        previous_output_extent.width != static_cast<uint32_t>(desired_output_key.width) ||
        previous_output_extent.height != static_cast<uint32_t>(desired_output_key.height);
    if (output_extent_changed) {
        // TextPass follows the same stable-resource rule as CustomShaderPass: keep the framebuffer
        // when only sampled atlas/input content changes, but release it before TextureCache is
        // allowed to replace a resized bridge output image.
        m_desc.framebuffer.reset();
    }
    auto output = device.tex_cache().Query(m_desc.output,
                                           ToTexKey(output_it->second),
                                           !output_it->second.allowReuse);
    if (!output.has_value()) {
        setPrepared(false);
        return;
    }
    m_desc.vk_output = output.value();
    const bool output_view_changed = previous_output_view != m_desc.vk_output.view;
    const bool framebuffer_missing = !m_desc.framebuffer;
    if (framebuffer_missing || output_extent_changed || output_view_changed) {
        if (!recreateFramebuffer(device)) {
            setPrepared(false);
            return;
        }
    }
}

void TextPass::execute(const Device& device, RenderingResources& rr) {
    auto* node = m_desc.node;
    auto* primitive = node != nullptr ? node->Text() : nullptr;
    if (primitive == nullptr) return;
    if (node != nullptr && !node->Visible() && !m_desc.execute_when_hidden) return;

    if (primitive->atlas_version != m_loaded_atlas_version ||
        m_desc.page_textures.size() != primitive->glyph_pages.size()) {
        // Text atlas content is owned by the scene primitive, not by render-graph pass creation.
        // Runtime text updates can therefore swap atlas pages or change page counts without a
        // graph rebuild. Refreshing the bound atlas images lazily here keeps the dedicated text
        // pass on the new scene-owned source of truth instead of depending on parser-time texture
        // registration.
        if (!refreshTextures(device)) return;
    }

    if (primitive->background_mesh != nullptr &&
        !ensureMeshBuffers(*primitive->background_mesh, m_background_buffers, rr)) {
        return;
    }
    if (m_page_buffers.size() != primitive->glyph_pages.size()) {
        m_page_buffers.resize(primitive->glyph_pages.size());
        for (auto& buffers : m_page_buffers) buffers.force_upload = true;
    }
    for (size_t page_index = 0; page_index < primitive->glyph_pages.size(); page_index++) {
        if (!ensureMeshBuffers(*primitive->glyph_pages[page_index].mesh, m_page_buffers[page_index], rr)) {
            return;
        }
    }

    auto write_uniforms = [&](const std::array<float, 4>& color) {
        TextPassUniforms uniforms {};
        if (m_desc.scene != nullptr && m_desc.scene->shaderValueUpdater != nullptr && node != nullptr) {
            sprite_map_t sprites;
            m_desc.scene->shaderValueUpdater->UpdateUniforms(
                node,
                sprites,
                [&uniforms](std::string_view name, wallpaper::ShaderValue value) {
                    if (name != wallpaper::G_MVP || value.size() < 16) return;
                    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
                    for (int column = 0; column < 4; column++) {
                        for (int row = 0; row < 4; row++) {
                            // ShaderValue uses a size_t index while the matrix loops are small
                            // signed integers; materializing the index keeps warning-clean builds
                            // without changing the column-major uniform contract.
                            const auto uniform_index = static_cast<size_t>(column * 4 + row);
                            matrix(row, column) = value[uniform_index];
                        }
                    }
                    WriteMatrixToUniform(uniforms, matrix);
                });
        }
        std::copy(color.begin(), color.end(), uniforms.color);
        rr.dyn_buf->writeToBuf(m_desc.ubo_buf,
                               { reinterpret_cast<uint8_t*>(const_cast<TextPassUniforms*>(&uniforms)),
                                 sizeof(uniforms) });
    };

    auto bind_uniforms = [&]() {
        VkDescriptorBufferInfo buffer_info {
            rr.dyn_buf->gpuBuf(),
            m_desc.ubo_buf.offset,
            m_desc.ubo_buf.size,
        };
        VkWriteDescriptorSet write {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &buffer_info,
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
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
        };
        rr.command.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, write);
    };

    const VkExtent2D output_extent {
        .width = m_desc.vk_output.extent.width,
        .height = m_desc.vk_output.extent.height,
    };
    VkRenderPassBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = *m_desc.pipeline.pass,
        .framebuffer = *m_desc.framebuffer,
        .renderArea = VkRect2D { .offset = { 0, 0 }, .extent = output_extent },
        .clearValueCount = 1,
        .pClearValues = &m_desc.clear_value,
    };
    rr.command.BeginRenderPass(begin_info, VK_SUBPASS_CONTENTS_INLINE);
    rr.command.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.handle);

    VkViewport viewport {
        .x = 0.0f,
        .y = static_cast<float>(m_desc.vk_output.extent.height),
        .width = static_cast<float>(m_desc.vk_output.extent.width),
        .height = -static_cast<float>(m_desc.vk_output.extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor { { 0, 0 }, output_extent };
    rr.command.SetViewport(0, viewport);
    rr.command.SetScissor(0, scissor);

    auto draw_mesh = [&](MeshBuffers& buffers, const ImageSlotsRef& texture, const std::array<float, 4>& color) {
        if (buffers.draw_count == 0) return;
        write_uniforms(color);
        bind_uniforms();
        bind_texture(texture);
        auto gpu_buf = rr.dyn_buf->gpuBuf();
        for (usize binding_index = 0; binding_index < buffers.vertex_bufs.size(); binding_index++) {
            auto& subref = buffers.vertex_bufs[binding_index];
            rr.command.BindVertexBuffers(static_cast<uint32_t>(binding_index), 1, &gpu_buf, &subref.offset);
        }
        // Glyph page meshes are indexed, while the optional opaque background is a plain strip.
        // Supporting both draw modes keeps the direct text primitive self-contained instead of
        // depending on the old generic image pass behavior for one half of the text renderable.
        if (buffers.index_buf) {
            rr.command.BindIndexBuffer(gpu_buf, buffers.index_buf.offset, VK_INDEX_TYPE_UINT16);
            rr.command.DrawIndexed(buffers.draw_count, 1, 0, 0, 0);
        } else {
            rr.command.Draw(buffers.draw_count, 1, 0, 0);
        }
    };

    if (primitive->object.opaquebackground && primitive->background_mesh != nullptr) {
        draw_mesh(m_background_buffers,
                  m_desc.background_texture,
                  ResolveTextColor(*primitive, true));
    }

    for (size_t page_index = 0; page_index < primitive->glyph_pages.size(); page_index++) {
        if (page_index >= m_desc.page_textures.size()) break;
        draw_mesh(m_page_buffers[page_index],
                  m_desc.page_textures[page_index],
                  ResolveTextColor(*primitive, false));
    }

    rr.command.EndRenderPass();
}

void TextPass::destory(const Device&, RenderingResources& rr) {
    // Keep the cached text PSO alive through PipelineStateCache, but release every residency-bound
    // object that points at hidden-layer textures, render targets, or dynamic-buffer suballocations.
    m_desc.framebuffer.reset();
    m_desc.vk_output = {};
    m_desc.background_texture = {};
    m_desc.page_textures.clear();
    for (auto& subref : m_background_buffers.vertex_bufs) {
        rr.dyn_buf->unallocateSubRef(subref);
    }
    if (m_background_buffers.index_buf) rr.dyn_buf->unallocateSubRef(m_background_buffers.index_buf);
    m_background_buffers = {};
    for (auto& page_buffers : m_page_buffers) {
        for (auto& subref : page_buffers.vertex_bufs) {
            rr.dyn_buf->unallocateSubRef(subref);
        }
        if (page_buffers.index_buf) rr.dyn_buf->unallocateSubRef(page_buffers.index_buf);
    }
    m_page_buffers.clear();
    rr.dyn_buf->unallocateSubRef(m_desc.ubo_buf);
    m_desc.ubo_buf = {};
    setPrepared(false);
}
