#include "CustomShaderPass.hpp"
#include "scene/Scene.h"
#include "scene/SceneShader.h"
#include "scene/SceneTextPrimitive.h"

#include "SpecTexs.hpp"
#include "vulkan/Shader.hpp"
#include "vulkan/VideoTextureCache.hpp"
#include "vvk/vma_wrapper.hpp"
#include "utils/Logging.h"
#include "utils/AutoDeletor.hpp"
#include "Resource.hpp"
#include "PassCommon.hpp"
#include "interface/IImageParser.h"

#include "core/ArrayHelper.hpp"
#include "core/StringHelper.hpp"

#include <cassert>
#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>

using namespace wallpaper::vulkan;

namespace
{
std::string CustomShaderPipelineCompatibilityKey(VkAttachmentLoadOp load_op, bool model_pass,
                                                 VkAttachmentLoadOp depth_load_op) {
    // Keep this key limited to Vulkan render-pass compatibility. GraphicsPipeline adds the shader,
    // descriptor, vertex-input, blend, depth, and topology state to the final cache key, matching
    // the descriptor-driven PSO caches used by larger renderers instead of tying immutable PSOs to
    // a transient layer/pass identity.
    return "CustomShaderPass|format=rgba8|final=shader-read|load=" +
           std::to_string(static_cast<int>(load_op)) +
           "|model=" + (model_pass ? std::string("1") : std::string("0")) +
           "|depth-format=d32|depth-load=" + std::to_string(static_cast<int>(depth_load_op));
}

void PopulateTextureBindingsFromReflection(wallpaper::vulkan::CustomShaderPass::Desc& desc,
                                           const wallpaper::vulkan::ShaderReflected&  ref,
                                           size_t texture_count) {
    desc.vk_tex_binding.clear();
    desc.vk_tex_binding.reserve(texture_count);
    for (size_t i = 0; i < texture_count; i++) {
        wallpaper::i32 binding { -1 };
        if (i < wallpaper::WE_GLTEX_NAMES.size() &&
            wallpaper::exists(ref.binding_map, wallpaper::WE_GLTEX_NAMES[i])) {
            binding = static_cast<wallpaper::i32>(
                ref.binding_map.at(wallpaper::WE_GLTEX_NAMES[i]).binding);
        }
        desc.vk_tex_binding.push_back(binding);
    }
}

std::optional<VmaImageParameters> CreateModelDepthImage(const Device& device, VkExtent3D extent) {
    // Model depth is allocated only for opt-in 3D model passes. The existing 2D render-target cache
    // remains color-only, while separate model chunk passes can still behave like one depth-tested
    // scene when they share the same output texture.
    VmaImageParameters image;
    VkImageCreateInfo  info {
        .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext                 = nullptr,
        .imageType             = VK_IMAGE_TYPE_2D,
        .format                = VK_FORMAT_D32_SFLOAT,
        .extent                = extent,
        .mipLevels             = 1,
        .arrayLayers           = 1,
        .samples               = VK_SAMPLE_COUNT_1_BIT,
        .tiling                = VK_IMAGE_TILING_OPTIMAL,
        .usage                 = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    image.extent       = extent;
    image.mipmap_level = 1;

    VmaAllocationCreateInfo vma_info {};
    vma_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VVK_CHECK_ACT(return std::nullopt,
                         vvk::CreateImage(device.vma_allocator(), info, vma_info, image.handle));

    VkImageViewCreateInfo view_info {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .image    = *image.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_D32_SFLOAT,
        .subresourceRange =
            VkImageSubresourceRange {
                .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };
    VVK_CHECK_ACT(return std::nullopt, device.handle().CreateImageView(view_info, image.view));
    return image;
}

} // namespace

CustomShaderPass::CustomShaderPass(const Desc& desc) {
    // The render graph builder already classifies hidden offscreen dependencies and gives passes
    // a live scene pointer for diagnostics. Preserve that prepared intent here; dropping these
    // fields forced text/effect passes back through generic visibility and null-scene behavior.
    m_desc.scene               = desc.scene;
    m_desc.node                = desc.node;
    m_desc.layer_id            = desc.layer_id;
    m_desc.execute_when_hidden = desc.execute_when_hidden;
    m_desc.should_execute      = desc.should_execute;
    m_desc.textures            = desc.textures;
    m_desc.output              = desc.output;
    m_desc.force_alpha_write   = desc.force_alpha_write;
    m_desc.premultiplied_source_blend = desc.premultiplied_source_blend;
    m_desc.clear_before_draw   = desc.clear_before_draw;
    m_desc.camera_override     = desc.camera_override;
    m_desc.use_active_camera_for_uniforms = desc.use_active_camera_for_uniforms;
    m_desc.use_active_camera_for_parallax = desc.use_active_camera_for_parallax;
    m_desc.sprites_map         = desc.sprites_map;
    m_desc.model_pass          = desc.model_pass;
    m_desc.depth_test          = desc.depth_test;
    m_desc.depth_write         = desc.depth_write;
    m_desc.clear_depth         = desc.clear_depth;
};
CustomShaderPass::~CustomShaderPass() {}

std::string CustomShaderPass::residencyKey() const {
    return "CustomShaderPass|layer=" + std::to_string(m_desc.layer_id) +
           "|node=" + std::to_string(reinterpret_cast<std::uintptr_t>(m_desc.node)) +
           "|output=" + m_desc.output;
}

bool CustomShaderPass::canReuseForResidency(const VulkanPass& next_pass) const {
    const auto* next = dynamic_cast<const CustomShaderPass*>(&next_pass);
    if (next == nullptr) return false;
    // A prepared pass may be reused only when its immutable GPU contract is the same. Runtime
    // visibility gates and descriptor texture keys can be refreshed in place, but changing model
    // depth/blend state or the owning SceneNode would require a different render pass/pipeline.
    return residencyKey() == next->residencyKey() &&
           m_desc.execute_when_hidden == next->m_desc.execute_when_hidden &&
           m_desc.model_pass == next->m_desc.model_pass &&
           m_desc.depth_test == next->m_desc.depth_test &&
           m_desc.depth_write == next->m_desc.depth_write &&
           m_desc.clear_depth == next->m_desc.clear_depth &&
           // Forced alpha changes the prepared pipeline's color write mask and alpha blend factors.
           // Reusing a pass across that boundary would keep the old alpha contract alive even after
           // the render graph reroutes a composition child into a transparent source target.
           m_desc.force_alpha_write == next->m_desc.force_alpha_write &&
           m_desc.premultiplied_source_blend ==
               next->m_desc.premultiplied_source_blend &&
           m_desc.clear_before_draw == next->m_desc.clear_before_draw &&
           // The uniform update lambda captures the pass camera route. Treat it as part of the
           // prepared uniform contract so a source-space composition route cannot reuse a screen-space
           // publisher after a topology rebuild.
           m_desc.camera_override == next->m_desc.camera_override &&
           m_desc.use_active_camera_for_uniforms ==
               next->m_desc.use_active_camera_for_uniforms &&
           m_desc.use_active_camera_for_parallax ==
               next->m_desc.use_active_camera_for_parallax &&
           m_desc.textures.size() == next->m_desc.textures.size();
}

void CustomShaderPass::absorbResidencyGraphState(const VulkanPass& next_pass) {
    const auto* next = dynamic_cast<const CustomShaderPass*>(&next_pass);
    if (next == nullptr) return;
    // Render-graph diffing keeps this pass's expensive Vulkan objects alive while replacing only
    // the declarative state that can change as layers move between hidden and visible residency.
    // Texture handles are rebound by refreshResources()/prepare(), and the runtime gate must follow
    // the newly built graph so effect bypass/final-composite branches stay correct.
    m_desc.scene          = next->m_desc.scene;
    m_desc.layer_id       = next->m_desc.layer_id;
    m_desc.should_execute = next->m_desc.should_execute;
    m_desc.textures       = next->m_desc.textures;
    m_desc.output         = next->m_desc.output;
    m_desc.force_alpha_write = next->m_desc.force_alpha_write;
    m_desc.premultiplied_source_blend = next->m_desc.premultiplied_source_blend;
    m_desc.clear_before_draw = next->m_desc.clear_before_draw;
    m_desc.camera_override = next->m_desc.camera_override;
    m_desc.use_active_camera_for_uniforms = next->m_desc.use_active_camera_for_uniforms;
    m_desc.use_active_camera_for_parallax = next->m_desc.use_active_camera_for_parallax;
    m_desc.sprites_map    = next->m_desc.sprites_map;
}

bool CustomShaderPass::referencesRenderTarget(std::string_view render_target) const {
    // Custom shader passes are affected when either their output framebuffer is the dirty target or
    // one of their descriptor inputs samples it. This lets a resized text bridge update the exact
    // effect chain that consumes it instead of refreshing every other shader in the wallpaper.
    if (m_desc.output == render_target) return true;
    for (const auto& texture : m_desc.textures) {
        if (texture == render_target) return true;
    }
    return false;
}

std::optional<vvk::RenderPass>
CreateRenderPass(const vvk::Device& device, VkFormat format, VkAttachmentLoadOp loadOp,
                 VkImageLayout finalLayout, bool withDepth = false,
                 VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR) {
    VkAttachmentDescription attachment {
        .format         = format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = loadOp, // VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = finalLayout, // ShaderReadOnlyOptimal
    };

    if (loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
        attachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkAttachmentReference attachment_ref {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentDescription depth_attachment {
        .format         = VK_FORMAT_D32_SFLOAT,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = depthLoadOp,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = depthLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD
                              ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference depth_attachment_ref {
        .attachment = 1,
        .layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    std::array<VkAttachmentDescription, 2> attachments { attachment, depth_attachment };

    VkSubpassDescription subpass {
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount    = 1,
        .pColorAttachments       = &attachment_ref,
        .pDepthStencilAttachment = withDepth ? &depth_attachment_ref : nullptr,
    };

    VkSubpassDependency dependency {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask =
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = {},
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo creatinfo {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = withDepth ? 2u : 1u,
        .pAttachments    = attachments.data(),
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dependency,
    };
    vvk::RenderPass pass;
    if (auto res = device.CreateRenderPass(creatinfo, pass); res == VK_SUCCESS) {
        return pass;
    } else {
        VVK_CHECK(res);
        return std::nullopt;
    }
}

static void UpdateUniform(StagingBuffer* buf, const StagingBufferRef& bufref,
                          const ShaderReflected::Block& block, std::string_view name,
                          const wallpaper::ShaderValue& value) {
    using namespace wallpaper;
    std::span<uint8_t> value_u8 { (uint8_t*)value.data(),
                                  value.size() * sizeof(ShaderValue::value_type) };
    auto               uni = block.member_map.find(name);
    if (uni == block.member_map.end()) {
        // log
        return;
    }

    const size_t offset        = uni->second.offset;
    const size_t reflectedSize = uni->second.size;
    const size_t packedSize    = value_u8.size();

    if (reflectedSize < packedSize) {
        // Some first-party model shaders declare compact matrix uniforms such as `mat3
        // g_ModelMatrix`, while the runtime updater naturally owns a full 4x4 scene transform.
        // Writing the whole packed matrix would overflow the reflected member slot and corrupt the
        // following uniforms. Clamp only at the upload boundary so 2D shader generation and the
        // shared ShaderValue representation do not need a model-specific branch.
        buf->writeToBuf(bufref, value_u8.subspan(0, reflectedSize), offset);
        return;
    }

    if (reflectedSize == packedSize || value.size() <= 1) {
        buf->writeToBuf(bufref, value_u8, offset);
        return;
    }

    // SPIR-V reflection reports std140 array sizes for uniforms such as
    // `float g_AudioSpectrum32Left[32]`, which occupy 16 bytes per element.
    // Our runtime values are stored densely as `float[N]`, so copy them using
    // the reflected stride instead of writing the packed blob directly.
    if (reflectedSize > packedSize && reflectedSize % value.size() == 0) {
        const size_t stride = reflectedSize / value.size();
        if (stride >= sizeof(ShaderValue::value_type)) {
            for (size_t i = 0; i < value.size(); i++) {
                std::span<uint8_t> elem { reinterpret_cast<uint8_t*>(
                                              const_cast<ShaderValue::value_type*>(&value[i])),
                                          sizeof(ShaderValue::value_type) };
                buf->writeToBuf(bufref, elem, offset + i * stride);
            }
            return;
        }
    }

    buf->writeToBuf(bufref, value_u8, offset);
}

static void WriteMaterialUniforms(StagingBuffer* buf, const StagingBufferRef& bufref,
                                  const ShaderReflected::Block&   block,
                                  const wallpaper::SceneMaterial& material) {
    auto write_values = [&](const auto& values) {
        for (const auto& [name, value] : values) {
            if (! wallpaper::exists(block.member_map, name)) continue;
            UpdateUniform(buf, bufref, block, name, value);
        }
    };

    if (material.customShader.shader != nullptr) {
        write_values(material.customShader.shader->default_uniforms);
    }
    write_values(material.customShader.constValues);
}

void LogStagingAllocRequest(std::string_view kind, const CustomShaderPass::Desc& desc,
                            VkDeviceSize size) {
    LOG_VERBOSE("StagingAllocRequest: kind=%.*s node='%s' camera='%s' shader='%s' output='%s' "
             "size=%zu dyn=%s",
             static_cast<int>(kind.size()),
             kind.data(),
             desc.node ? desc.node->Name().c_str() : "<null>",
             desc.node ? desc.node->Camera().c_str() : "<null>",
             desc.node && desc.node->Mesh() && desc.node->Mesh()->Material() &&
                     desc.node->Mesh()->Material()->customShader.shader
                 ? desc.node->Mesh()->Material()->customShader.shader->name.c_str()
                 : "<null>",
             desc.output.c_str(),
             static_cast<size_t>(size),
             desc.dyn_vertex ? "true" : "false");
}

constexpr VkDeviceSize kInitialDynamicSuballocationSize = 64 * 1024;
constexpr VkDeviceSize kDynamicIndexQuadFloorSize       = sizeof(uint16_t) * 6;

VkDeviceSize InitialDynamicSuballocationSize(VkDeviceSize capacity, VkDeviceSize live_size,
                                             VkDeviceSize element_size) {
    if (capacity == 0) return 0;

    const VkDeviceSize non_empty_element = std::max<VkDeviceSize>(element_size, 1);
    const VkDeviceSize required_live     = std::max<VkDeviceSize>(live_size, non_empty_element);
    const VkDeviceSize bootstrap         = std::min<VkDeviceSize>(
        capacity, std::max<VkDeviceSize>(kInitialDynamicSuballocationSize, non_empty_element));

    // Dynamic particle meshes often advertise a very large theoretical capacity while starting
    // with zero live vertices. Reserve only a small bootstrap range up front, but never choose a
    // range smaller than the data that is already live and must be uploaded immediately.
    return std::min<VkDeviceSize>(capacity, std::max(required_live, bootstrap));
}

VkDeviceSize DynamicVertexUploadSize(const wallpaper::SceneVertexArray& vertex) {
    // Vertex arrays expose both live bytes and authored capacity. Use the live byte count for the
    // first upload so character-rain style particle systems do not reserve their entire theoretical
    // maximum before any spawned particles exist.
    return InitialDynamicSuballocationSize(static_cast<VkDeviceSize>(vertex.CapacitySizeOf()),
                                           static_cast<VkDeviceSize>(vertex.DataSizeOf()),
                                           static_cast<VkDeviceSize>(vertex.OneSizeOf()));
}

VkDeviceSize DynamicIndexUploadSize(const wallpaper::SceneIndexArray& indice) {
    // Index buffers follow the same bootstrap rule as vertices, but CustomShaderPass binds them as
    // VK_INDEX_TYPE_UINT16 at draw time. SceneIndexArray stores both 32-bit model indices and
    // packed 16-bit particle indices behind the same byte-count API, so the non-empty dynamic floor
    // must match the GPU binding size. The effect-dependency route added for private image
    // composites can expose one-quad particle helpers with only 12 bytes of authored capacity;
    // using a 24-byte uint32_t floor makes those valid helpers fail before their first dynamic
    // upload.
    return InitialDynamicSuballocationSize(static_cast<VkDeviceSize>(indice.CapacitySizeof()),
                                           static_cast<VkDeviceSize>(indice.DataSizeOf()),
                                           kDynamicIndexQuadFloorSize);
}

VkCullModeFlags ToVkCullMode(wallpaper::SceneCullMode mode) {
    switch (mode) {
    case wallpaper::SceneCullMode::None: return VK_CULL_MODE_NONE;
    case wallpaper::SceneCullMode::Back: return VK_CULL_MODE_BACK_BIT;
    case wallpaper::SceneCullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    }
    return VK_CULL_MODE_NONE;
}

wallpaper::SceneCullMode ResolveModelCullMode(wallpaper::SceneCullMode mode,
                                              bool                     mirrored_handedness) {
    if (! mirrored_handedness) return mode;

    // A floor reflection uses a negative scale, so the model transform changes handedness and the
    // authored triangle winding is observed backwards by Vulkan. Flip only front/back model culling
    // here; `None` stays double-sided for receiver materials such as reflection grids, and
    // non-model custom shader passes never carry SceneModelRenderState at all.
    switch (mode) {
    case wallpaper::SceneCullMode::Back: return wallpaper::SceneCullMode::Front;
    case wallpaper::SceneCullMode::Front: return wallpaper::SceneCullMode::Back;
    case wallpaper::SceneCullMode::None: return wallpaper::SceneCullMode::None;
    }
    return mode;
}

bool ShouldWriteCustomShaderAlpha(const wallpaper::SceneMaterial& material,
                                  std::string_view                camera_name,
                                  bool                             force_alpha_write) {
    const bool is_model_pass = material.modelRenderState.has_value();
    // Model shaders may output non-opaque alpha for their own material math even when the authored
    // object is visually opaque. Allowing that alpha into `_rt_default` makes FinPass present a
    // translucent frame and visually crushes the lighting. Keep the RGB blend factors intact for
    // translucent model materials, but preserve the target alpha just like global 2D passes.
    if (is_model_pass) return false;

    // Only neutral publisher passes may opt in to camera-less alpha writes. Authored effect shaders
    // such as auto_sway can legally output alpha=1 for helper regions; letting those shaders write
    // alpha directly into a parent composition source turns the source into a dark matte instead of
    // the resolved child texture.
    if (force_alpha_write) return true;

    return ! (camera_name.empty() || wallpaper::sstart_with(camera_name, "global"));
}

std::string_view EffectiveCustomShaderCamera(
    const wallpaper::vulkan::CustomShaderPass::Desc& desc) {
    if (! desc.camera_override.empty()) return desc.camera_override;
    return desc.node != nullptr ? std::string_view(desc.node->Camera()) : std::string_view {};
}

void PreservePublisherAlpha(bool             source_over_alpha,
                                      bool             writes_alpha,
                                      VkPipelineColorBlendAttachmentState& blend_state) {
    if (! source_over_alpha || ! writes_alpha || ! blend_state.blendEnable) {
        return;
    }

    // Neutral publisher passes are the last step that places a resolved private effect texture into
    // a composition source. Keep their RGB blend authored, but compose alpha as source-over coverage
    // so the parent effect receives the child's real silhouette.
    blend_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
}

void ApplyPremultipliedSourceBlend(
    const wallpaper::vulkan::CustomShaderPass::Desc& desc,
    bool                                             writes_alpha,
    VkPipelineColorBlendAttachmentState&             blend_state) {
    if (! desc.premultiplied_source_blend || ! blend_state.blendEnable) return;

    // The private layer-surface puppet pass has already composited straight-alpha texture samples
    // into a transparent render target. Its sampled RGB is premultiplied, while alpha remains the
    // correct source-over coverage. Keep the authored destination factor from the material blend
    // mode, but consume RGB as premultiplied color so the final publisher does not fade animated
    // eye/eyelid pixels before the puppet mesh actually covers them.
    blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    if (writes_alpha) {
        blend_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
}

void LogForcedAlphaPolicy(const wallpaper::vulkan::CustomShaderPass::Desc& desc,
                          const wallpaper::SceneMaterial&                  material,
                          std::string_view                                 camera_name,
                          bool                                             writes_alpha,
                          VkColorComponentFlags                            color_mask) {
    if (! desc.force_alpha_write) return;

    LOG_VERBOSE("CustomShaderForceAlphaPolicy: layer=%d node='%s' camera='%.*s' material='%s' "
             "output='%s' writes-alpha=%s color-mask=0x%x blend=%d premultiplied-source=%s",
             desc.layer_id,
             desc.node != nullptr ? desc.node->Name().c_str() : "",
             static_cast<int>(camera_name.size()),
             camera_name.data(),
             material.name.c_str(),
             desc.output.c_str(),
             writes_alpha ? "true" : "false",
             static_cast<unsigned>(color_mask),
             static_cast<int>(material.blenmode),
             desc.premultiplied_source_blend ? "true" : "false");
}

void ApplyModelPassDesc(const wallpaper::SceneMaterial&            material,
                        wallpaper::vulkan::CustomShaderPass::Desc& desc,
                        VkAttachmentLoadOp&                        load_op) {
    const auto& model_state = material.modelRenderState;
    if (! model_state.has_value()) return;

    desc.model_pass  = true;
    desc.depth_test  = model_state->depthTest;
    desc.depth_write = model_state->depthWrite;
    // Model passes are the only custom-shader passes allowed to override the historical load/cull
    // defaults. The parser chooses a color-load mode per output target, so offscreen model buffers
    // can be cleared once per frame before later chunks load and composite into the same image.
    switch (model_state->colorLoadMode) {
    case wallpaper::SceneModelColorLoadMode::DontCare: break;
    case wallpaper::SceneModelColorLoadMode::Load: load_op = VK_ATTACHMENT_LOAD_OP_LOAD; break;
    case wallpaper::SceneModelColorLoadMode::Clear: load_op = VK_ATTACHMENT_LOAD_OP_CLEAR; break;
    }
}

std::string_view ModelColorLoadModeName(wallpaper::SceneModelColorLoadMode mode) {
    switch (mode) {
    case wallpaper::SceneModelColorLoadMode::DontCare: return "dont-care";
    case wallpaper::SceneModelColorLoadMode::Load: return "load";
    case wallpaper::SceneModelColorLoadMode::Clear: return "clear";
    }
    return "unknown";
}

VkClearValue BuildCustomShaderClearValue(const wallpaper::Scene&         scene,
                                         const wallpaper::SceneMaterial& material,
                                         bool                            transparent_clear) {
    if (transparent_clear) {
        return VkClearValue {
            .color = { 0.0f, 0.0f, 0.0f, 0.0f },
        };
    }

    if (material.modelRenderState.has_value() &&
        material.modelRenderState->colorLoadMode == wallpaper::SceneModelColorLoadMode::Clear) {
        // Model-only offscreen targets are sampled as textures by later passes. Transparent black
        // is the neutral clear value for those buffers: uncovered pixels contribute no stale color,
        // no alpha, and no previous-frame reflection when the current model geometry shrinks.
        return VkClearValue {
            .color = { 0.0f, 0.0f, 0.0f, 0.0f },
        };
    }

    auto& sc = scene.clearColor;
    // Non-model and main-target custom shader passes retain the existing scene clear color
    // contract. Keeping this branch shared avoids changing ordinary image/effect behavior while
    // still letting model state opt into transparent offscreen clears explicitly.
    return VkClearValue {
        .color = { sc[0], sc[1], sc[2], 1.0f },
    };
}

void ApplyExplicitClearPolicy(const wallpaper::vulkan::CustomShaderPass::Desc& desc,
                              const wallpaper::SceneMaterial&                  material,
                              VkAttachmentLoadOp&                              load_op) {
    if (!desc.clear_before_draw) return;

    load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    LOG_VERBOSE("CustomShaderExplicitClearPolicy: layer=%d node='%s' material='%s' "
             "output='%s'",
             desc.layer_id,
             desc.node != nullptr ? desc.node->Name().c_str() : "",
             material.name.c_str(),
             desc.output.c_str());
}

struct CustomShaderRenderState {
    VkPipelineColorBlendAttachmentState color_blend {};
    VkAttachmentLoadOp                  color_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
};

CustomShaderRenderState BuildCustomShaderRenderState(
    const wallpaper::SceneMaterial& material, wallpaper::vulkan::CustomShaderPass::Desc& desc) {
    CustomShaderRenderState state;
    VkColorComponentFlags   color_mask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    const auto camera_name = EffectiveCustomShaderCamera(desc);
    const bool writes_alpha = ShouldWriteCustomShaderAlpha(material, camera_name, desc.force_alpha_write);

    if (writes_alpha) color_mask |= VK_COLOR_COMPONENT_A_BIT;
    state.color_blend.colorWriteMask = color_mask;

    const auto blend_mode = material.blenmode;
    SetBlend(blend_mode, state.color_blend);
    ApplyPremultipliedSourceBlend(desc, writes_alpha, state.color_blend);
    PreservePublisherAlpha(desc.force_alpha_write, writes_alpha, state.color_blend);
    desc.blending = state.color_blend.blendEnable;

    SetAttachmentLoadOp(blend_mode, state.color_load_op);
    ApplyModelPassDesc(material, desc, state.color_load_op);
    ApplyExplicitClearPolicy(desc, material, state.color_load_op);
    LogForcedAlphaPolicy(desc, material, camera_name, writes_alpha, color_mask);
    if (WallpaperDebugLayerEnabled(desc.layer_id)) {
        std::ostringstream textures_oss;
        textures_oss << "[";
        for (size_t i = 0; i < desc.textures.size(); i++) {
            if (i != 0) textures_oss << ", ";
            textures_oss << desc.textures[i];
        }
        textures_oss << "]";
        LOG_INFO("DebugLayerShaderState: layer=%d node='%s' material='%s' output='%s' "
                 "blend=%d blending=%s load-op=%d force-alpha=%s clear-before=%s "
                 "premultiplied-source=%s textures=%s",
                 desc.layer_id,
                 desc.node != nullptr ? desc.node->Name().c_str() : "",
                 material.name.c_str(),
                 desc.output.c_str(),
                 static_cast<int>(material.blenmode),
                 desc.blending ? "true" : "false",
                 static_cast<int>(state.color_load_op),
                 desc.force_alpha_write ? "true" : "false",
                 desc.clear_before_draw ? "true" : "false",
                 desc.premultiplied_source_blend ? "true" : "false",
                 textures_oss.str().c_str());
    }
    return state;
}

void ApplyModelPipelineState(const wallpaper::SceneMaterial&                  material,
                             const wallpaper::vulkan::CustomShaderPass::Desc& desc,
                             GraphicsPipeline&                                pipeline) {
    const auto& model_state = material.modelRenderState;
    if (! model_state.has_value()) return;

    // Only model materials can carry this optional state. Applying it here keeps culling separate
    // from the old 2D custom-shader defaults while still using the existing pipeline construction
    // path for shader reflection, descriptors, and mesh buffers.
    pipeline.depth.depthTestEnable       = desc.depth_test;
    pipeline.depth.depthWriteEnable      = desc.depth_write;
    pipeline.depth.depthCompareOp        = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipeline.depth.depthBoundsTestEnable = false;
    pipeline.depth.stencilTestEnable     = false;
    const auto effective_cull_mode =
        ResolveModelCullMode(model_state->cullMode, model_state->mirroredHandedness);
    pipeline.raster.cullMode = ToVkCullMode(effective_cull_mode);
    LOG_VERBOSE("ModelRenderStateBind: node='%s' shader='%s' output='%s' color-load=%s "
             "mirrored-handedness=%s depth-test=%s depth-write=%s depth-clear=%s cull=%u",
             desc.node != nullptr ? desc.node->Name().c_str() : "<null>",
             material.customShader.shader != nullptr ? material.customShader.shader->name.c_str()
                                                     : "<null>",
             desc.output.c_str(),
             ModelColorLoadModeName(model_state->colorLoadMode).data(),
             model_state->mirroredHandedness ? "true" : "false",
             desc.depth_test ? "true" : "false",
             desc.depth_write ? "true" : "false",
             desc.clear_depth ? "true" : "false",
             static_cast<unsigned>(pipeline.raster.cullMode));
}

VkDeviceSize GrowDynamicSuballocationSize(VkDeviceSize current_size,
                                          VkDeviceSize required_live_size, VkDeviceSize capacity,
                                          VkDeviceSize element_size) {
    if (capacity == 0) return 0;

    // Growth is geometric but clamped to authored capacity. That keeps normal particle expansion
    // amortized while still refusing to cross the renderer-side maximum promised by the scene data.
    VkDeviceSize next_size =
        current_size == 0
            ? InitialDynamicSuballocationSize(capacity, required_live_size, element_size)
            : current_size;
    const VkDeviceSize required = std::min<VkDeviceSize>(
        capacity,
        std::max<VkDeviceSize>(required_live_size, std::max<VkDeviceSize>(element_size, 1)));

    while (next_size < required && next_size < capacity) {
        const VkDeviceSize doubled = next_size > capacity / 2 ? capacity : next_size * 2;
        next_size                  = std::max<VkDeviceSize>(required, doubled);
        next_size                  = std::min<VkDeviceSize>(next_size, capacity);
    }
    return next_size;
}

bool RefreshCustomShaderPassTextures(wallpaper::Scene& scene, const Device& device,
                                     CustomShaderPass::Desc& desc) {
    desc.vk_textures.resize(desc.textures.size());
    for (wallpaper::usize i = 0; i < desc.textures.size(); i++) {
        auto& tex_name = desc.textures[i];
        if (tex_name.empty()) {
            desc.vk_textures[i] = {};
            continue;
        }

        ImageSlotsRef img_slots;
        const auto    render_target_it = scene.renderTargets.find(tex_name);
        if (render_target_it != scene.renderTargets.end()) {
            // The scene render-target table is the authoritative source for internal effect FBOs.
            // Some authored blur chains use plain names like `blur_start_2_<addr>`, so relying only
            // on the `_rt_` prefix would send valid runtime targets through the material-file
            // parser.
            auto& rt  = render_target_it->second;
            auto  opt = device.tex_cache().Query(
                tex_name, wallpaper::vulkan::ToTexKey(rt), ! rt.allowReuse);
            if (! opt.has_value()) {
                LOG_ERROR("CustomShaderPassRefresh: query input failed node='%s' output='%s' "
                          "slot=%zu texture='%s'",
                          desc.node != nullptr ? desc.node->Name().c_str() : "<null>",
                          desc.output.c_str(),
                          static_cast<size_t>(i),
                          tex_name.c_str());
                desc.vk_textures[i] = {};
                continue;
            }
            img_slots.slots.clear();
            img_slots.slots.push_back(opt.value());
        } else if (wallpaper::IsSpecTex(tex_name)) {
            LOG_ERROR("CustomShaderPassRefresh: missing input render target node='%s' "
                      "output='%s' slot=%zu texture='%s'",
                      desc.node != nullptr ? desc.node->Name().c_str() : "<null>",
                      desc.output.c_str(),
                      static_cast<size_t>(i),
                      tex_name.c_str());
            desc.vk_textures[i] = {};
            continue;
        } else {
            if (scene.dirtyImportedTextureKeys.count(tex_name) == 0) {
                if (auto cached_slots = device.tex_cache().FindTex(tex_name);
                    cached_slots.has_value()) {
                    desc.vk_textures[i] = *cached_slots;
                    continue;
                }
            }

            const auto texture_it = scene.textures.find(tex_name);
            const bool static_scene_texture =
                texture_it != scene.textures.end() && ! texture_it->second.isVideo;
            auto image = static_scene_texture ? scene.GetParsedImageIfReady(tex_name) : nullptr;
            if (image == nullptr) {
                image = static_scene_texture && scene.dirtyImportedTextureKeys.count(tex_name) == 0
                            ? scene.ParseImageBlockingCached(tex_name)
                            : (scene.imageParser != nullptr ? scene.imageParser->Parse(tex_name)
                                                            : nullptr);
            }
            if (image) {
                if (scene.textures.count(tex_name) != 0 && scene.textures.at(tex_name).isVideo) {
                    const auto paused_it = scene.videoTexturePaused.find(tex_name);
                    const bool stopped   = scene.videoTextureStopped.count(tex_name) != 0;
                    // Hidden video passes are kept prepared so visibility flips are cheap, but the
                    // backing decoder should still start paused unless a scene script explicitly
                    // requested playback for this texture.
                    const bool initially_paused =
                        paused_it != scene.videoTexturePaused.end()
                            ? paused_it->second
                            : (desc.node != nullptr && ! desc.node->Visible());
                    const auto initial_state =
                        stopped
                            ? wallpaper::VideoTexturePlaybackState::Stopped
                            : (initially_paused ? wallpaper::VideoTexturePlaybackState::Paused
                                                : wallpaper::VideoTexturePlaybackState::Playing);
                    img_slots = device.video_tex_cache().Acquire(
                        tex_name, scene.textures.at(tex_name), *image, initial_state);
                } else {
                    img_slots = device.tex_cache().CreateTex(*image);
                    if (static_scene_texture) {
                        scene.DropParsedImageCache(tex_name);
                    }
                }
            } else {
                LOG_ERROR("parse tex \"%s\" failed", tex_name.c_str());
                desc.vk_textures[i] = {};
                continue;
            }
        }
        desc.vk_textures[i] = img_slots;
    }

    auto&      tex_name  = desc.output;
    const auto output_it = scene.renderTargets.find(tex_name);
    if (output_it == scene.renderTargets.end()) {
        // Outputs must be registered render targets, but they do not have to be `_rt_`-prefixed:
        // effect-local FBOs are uniquified from their authored names and are still valid Vulkan
        // framebuffer destinations once WPSceneParser has inserted them into scene.renderTargets.
        LOG_ERROR("CustomShaderPassRefresh: missing output render target node='%s' output='%s'",
                  desc.node != nullptr ? desc.node->Name().c_str() : "<null>",
                  tex_name.c_str());
        return false;
    }
    auto& rt = output_it->second;
    if (auto opt =
            device.tex_cache().Query(tex_name, wallpaper::vulkan::ToTexKey(rt), ! rt.allowReuse);
        opt.has_value()) {
        desc.vk_output = opt.value();
        return true;
    }
    LOG_ERROR("CustomShaderPassRefresh: query output failed node='%s' output='%s'",
              desc.node != nullptr ? desc.node->Name().c_str() : "<null>",
              tex_name.c_str());
    return false;
}

bool StaticSceneTexturesResidentForDeferredPrepare(wallpaper::Scene& scene, const Device& device,
                                                   const CustomShaderPass::Desc& desc) {
    std::vector<std::string_view> missing_textures;
    for (const auto& tex_name : desc.textures) {
        if (tex_name.empty()) continue;
        if (scene.renderTargets.count(tex_name) != 0 || wallpaper::IsSpecTex(tex_name)) continue;
        if (scene.dirtyImportedTextureKeys.count(tex_name) != 0) continue;

        const auto texture_it = scene.textures.find(tex_name);
        if (texture_it == scene.textures.end() || texture_it->second.isVideo) continue;
        if (! device.tex_cache().FindTex(tex_name).has_value()) {
            missing_textures.push_back(tex_name);
        }
    }

    if (missing_textures.empty()) return true;

    // This is the guardrail that makes runtime visibility behave like a game-engine streaming
    // system: a deferred pass is not allowed to fall back to the blocking texture creation path
    // inside RefreshCustomShaderPassTextures(). It will stay off the render graph's executable set
    // until requestDeferredPrepareResources() has finished the background parse and the budgeted
    // GPU residency work.
    std::string missing;
    for (const auto texture : missing_textures) {
        if (! missing.empty()) missing += ",";
        missing += texture;
    }
    LOG_VERBOSE("CustomShaderPassDeferredPrepareWaitTextures: node='%s' output='%s' missing='%s'",
             desc.node != nullptr ? desc.node->Name().c_str() : "<null>",
             desc.output.c_str(),
             missing.c_str());
    return false;
}

VmaImageParameters* QuerySharedModelDepthImage(const Device& device, RenderingResources& rr,
                                               CustomShaderPass::Desc& desc) {
    auto&      depth      = rr.model_depth_images[desc.output];
    const bool missing    = ! depth.view || ! depth.handle;
    const bool wrong_size = depth.extent.width != desc.vk_output.extent.width ||
                            depth.extent.height != desc.vk_output.extent.height ||
                            depth.extent.depth != desc.vk_output.extent.depth;
    if (missing || wrong_size) {
        // A single output can receive many model chunk passes. Recreate the shared depth image only
        // when the output extent changes, then later chunks can load the same depth written by the
        // earlier chunks in render-graph order.
        auto replacement = CreateModelDepthImage(device, desc.vk_output.extent);
        if (! replacement.has_value()) {
            LOG_ERROR("CustomShaderPassRefresh: cannot create shared model depth image node='%s' "
                      "output='%s' extent=[%u,%u]",
                      desc.node != nullptr ? desc.node->Name().c_str() : "<null>",
                      desc.output.c_str(),
                      desc.vk_output.extent.width,
                      desc.vk_output.extent.height);
            return nullptr;
        }
        depth = std::move(replacement.value());
    }
    return &depth;
}

bool RecreateCustomShaderPassFramebuffer(const Device& device, RenderingResources& rr,
                                         CustomShaderPass::Desc& desc) {
    desc.fb.reset();
    if (! desc.pipeline.pass || desc.vk_output.view == VK_NULL_HANDLE ||
        desc.vk_output.extent.width == 0 || desc.vk_output.extent.height == 0) {
        LOG_ERROR("CustomShaderPassRefresh: cannot recreate framebuffer node='%s' output='%s' "
                  "hasRenderPass=%s hasView=%s extent=[%u,%u]",
                  desc.node != nullptr ? desc.node->Name().c_str() : "<null>",
                  desc.output.c_str(),
                  desc.pipeline.pass ? "true" : "false",
                  desc.vk_output.view != VK_NULL_HANDLE ? "true" : "false",
                  desc.vk_output.extent.width,
                  desc.vk_output.extent.height);
        return false;
    }
    if (desc.model_pass) {
        desc.depth_image_ref = QuerySharedModelDepthImage(device, rr, desc);
        if (desc.depth_image_ref == nullptr) return false;
    } else {
        desc.depth_image_ref = nullptr;
    }
    std::array<VkImageView, 2> attachments {
        desc.vk_output.view,
        desc.depth_image_ref != nullptr && desc.depth_image_ref->view ? *desc.depth_image_ref->view
                                                                      : VK_NULL_HANDLE,
    };
    VkFramebufferCreateInfo info {
        .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext           = nullptr,
        .renderPass      = *desc.pipeline.pass,
        .attachmentCount = desc.model_pass ? 2u : 1u,
        .pAttachments    = attachments.data(),
        .width           = desc.vk_output.extent.width,
        .height          = desc.vk_output.extent.height,
        .layers          = 1,
    };
    return device.handle().CreateFramebuffer(info, desc.fb) == VK_SUCCESS;
}

void CustomShaderPass::prepare(Scene& scene, const Device& device, RenderingResources& rr) {
    // Prepared passes can survive resource-only refreshes, so keep the live scene pointer current
    // before binding render targets and before text/effect diagnostics read bridge metadata.
    m_desc.scene = &scene;
    // A retrying prepared pass may still own a framebuffer whose attachment points at the previous
    // TextureCache image view. Drop it before `Query()` can resize and destroy that output image,
    // otherwise Vulkan sees a framebuffer referencing a dead attachment during minute-rollover
    // text bridge updates.
    m_desc.fb.reset();
    m_desc.vk_tex_binding.clear();
    if (! RefreshCustomShaderPassTextures(scene, device, m_desc)) return;
    SceneMesh& mesh = *(m_desc.node->Mesh());

    std::vector<Uni_ShaderSpv> spvs;
    DescriptorSetInfo          descriptor_info;
    ShaderReflected            ref;
    {
        SceneShader& shader = *(mesh.Material()->customShader.shader);

        if (! GenReflect(shader.codes, spvs, ref)) {
            LOG_ERROR("gen spv reflect failed, %s", shader.name.c_str());
            return;
        }

        auto& bindings = descriptor_info.bindings;
        bindings.resize(ref.binding_map.size());

        /*
        LOG_INFO("----shader------");
        LOG_INFO("%s", shader.name.c_str());
        LOG_INFO("--inputs:");
        for (auto& i : ref.input_location_map) {
            LOG_INFO("%d %s", i.second, i.first.c_str());
        }
        LOG_INFO("--bindings:");
        */

        std::transform(
            ref.binding_map.begin(), ref.binding_map.end(), bindings.begin(), [](auto& item) {
                // LOG_INFO("%d %s", item.second.binding, item.first.c_str());
                return item.second;
            });

        PopulateTextureBindingsFromReflection(m_desc, ref, m_desc.vk_textures.size());
    }

    m_desc.draw_count = 0;
    std::vector<VkVertexInputBindingDescription>   bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
    {
        m_desc.dyn_vertex = mesh.Dynamic();
        // Dynamic meshes allocate fresh staging/GPU subranges every time the render graph is
        // recompiled. A static-looking text layer can therefore become "clean" long before a new
        // pass instance is created, which leaves the newly allocated buffer ranges uninitialized if
        // we only upload on `mesh.Dirty()`. Marking the pass for one mandatory upload keeps
        // long-lived text quads valid across unrelated render-graph rebuilds triggered by other
        // animated layers such as effect-backed clocks and dates.
        m_desc.force_dyn_upload = m_desc.dyn_vertex;
        m_desc.vertex_bufs.resize(mesh.VertexCount());

        for (uint i = 0; i < mesh.VertexCount(); i++) {
            const auto& vertex    = mesh.GetVertexArray(i);
            auto        attrs_map = vertex.GetAttrOffsetMap();

            VkVertexInputBindingDescription bind_desc {
                .binding   = i,
                .stride    = (uint32_t)vertex.OneSizeOf(),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };
            bind_descriptions.push_back(bind_desc);

            for (auto& item : ref.input_location_map) {
                auto&      name     = item.first;
                auto&      input    = item.second;
                const bool has_attr = exists(attrs_map, name);
                usize      offset   = has_attr ? attrs_map[name].offset : 0;

                VkVertexInputAttributeDescription attr_desc {
                    .location = input.location,
                    .binding  = i,
                    .format   = input.format,
                    .offset   = (u32)offset,
                };
                attr_descriptions.push_back(attr_desc);
            }
            {
                auto& buf = m_desc.vertex_bufs[i];
                if (! m_desc.dyn_vertex) {
                    if (vertex.CapacitySizeOf() >= 1024 * 1024) {
                        LogStagingAllocRequest("vertex-static", m_desc, vertex.CapacitySizeOf());
                    }
                    if (! rr.vertex_buf->allocateSubRef(vertex.CapacitySizeOf(), buf)) return;
                    if (! rr.vertex_buf->writeToBuf(buf, { (uint8_t*)vertex.Data(), buf.size }))
                        return;
                } else {
                    if (vertex.CapacitySizeOf() >= 1024 * 1024) {
                        LogStagingAllocRequest("vertex-dynamic", m_desc, vertex.CapacitySizeOf());
                    }
                    const auto initial_size = DynamicVertexUploadSize(vertex);
                    if (! rr.dyn_buf->allocateSubRef(initial_size, buf)) return;
                }
            }
            m_desc.draw_count += (u32)(vertex.DataSize() / vertex.OneSize());
        }

        if (mesh.IndexCount() > 0) {
            auto&  indice     = mesh.GetIndexArray(0);
            size_t count      = (indice.DataCount() * 2) / 3;
            m_desc.draw_count = (u32)count * 3;
            auto& buf         = m_desc.index_buf;
            if (! m_desc.dyn_vertex) {
                if (indice.CapacitySizeof() >= 1024 * 1024) {
                    LogStagingAllocRequest("index-static", m_desc, indice.CapacitySizeof());
                }
                if (! rr.vertex_buf->allocateSubRef(indice.CapacitySizeof(), buf)) return;
                if (! rr.vertex_buf->writeToBuf(buf, { (uint8_t*)indice.Data(), buf.size })) return;
            } else {
                if (indice.CapacitySizeof() >= 1024 * 1024) {
                    LogStagingAllocRequest("index-dynamic", m_desc, indice.CapacitySizeof());
                }
                const auto initial_size = DynamicIndexUploadSize(indice);
                if (! rr.dyn_buf->allocateSubRef(initial_size, buf)) return;
            }
        }
    }
    {
        auto render_state = BuildCustomShaderRenderState(*mesh.Material(), m_desc);
        auto opt = CreateRenderPass(device.handle(),
                                    VK_FORMAT_R8G8B8A8_UNORM,
                                    render_state.color_load_op,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    m_desc.model_pass,
                                    m_desc.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                       : VK_ATTACHMENT_LOAD_OP_LOAD);
        if (! opt.has_value()) return;
        auto& pass = opt.value();

        descriptor_info.push_descriptor = true;
        GraphicsPipeline pipeline;
        pipeline.toDefault();
        ApplyModelPipelineState(*mesh.Material(), m_desc, pipeline);
        m_desc.pipeline.debug_name =
            "CustomShaderPass[node=" +
            (m_desc.node != nullptr ? m_desc.node->Name() : std::string("(null)")) +
            ",output=" + m_desc.output + "]";
        pipeline.addDescriptorSetInfo(spanone { descriptor_info })
            .setColorBlendStates(spanone { render_state.color_blend })
            .setTopology(m_desc.index_buf ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                          : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
            .addInputBindingDescription(bind_descriptions)
            .addInputAttributeDescription(attr_descriptions);
        for (auto& spv : spvs) pipeline.addStage(std::move(spv));

        m_desc.pipeline.cache_key = CustomShaderPipelineCompatibilityKey(
            render_state.color_load_op,
            m_desc.model_pass,
            m_desc.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
        if (! pipeline.create(device, pass, m_desc.pipeline, rr.pipeline_cache.get())) return;
    }
    {
        // The helper above already converts framebuffer creation into a plain success/failure
        // contract so that both the initial prepare path and the lightweight resource-refresh path
        // can share the same code. Keeping it as an explicit boolean check avoids routing a
        // non-VkResult helper through the `VVK_CHECK_*` macros, which only understand raw Vulkan
        // return codes.
        if (! RecreateCustomShaderPassFramebuffer(device, rr, m_desc)) return;
    }

    if (! ref.blocks.empty()) {
        auto& block = ref.blocks.front();
        if (WallpaperDebugLayerEnabled(m_desc.layer_id)) {
            std::ostringstream audio_members_oss;
            bool               first_audio_member = true;
            for (const auto& [member_name, member] : block.member_map) {
                (void)member;
                if (member_name.find("AudioSpectrum") == std::string::npos) continue;
                if (! first_audio_member) audio_members_oss << ", ";
                first_audio_member = false;
                audio_members_oss << member_name;
            }
            LOG_INFO("DebugSpectrumReflectedMembers: layer=%d shader='%s' block-size=%zu "
                     "audio-members=[%s]",
                     m_desc.layer_id,
                     mesh.Material() != nullptr && mesh.Material()->customShader.shader != nullptr
                         ? mesh.Material()->customShader.shader->name.c_str()
                         : "<null>",
                     block.size,
                     audio_members_oss.str().c_str());
        }
        if (block.size >= 1024 * 1024) {
            LogStagingAllocRequest("ubo", m_desc, block.size);
        }
        rr.dyn_buf->allocateSubRef(
            block.size, m_desc.ubo_buf, device.limits().minUniformBufferOffsetAlignment);
    }

    if (! ref.blocks.empty()) {
        std::function<void()> update_dyn_buf_op;
        if (m_desc.dyn_vertex) {
            auto&       mesh             = *m_desc.node->Mesh();
            auto*       dyn_buf          = rr.dyn_buf;
            auto&       vertex_bufs      = m_desc.vertex_bufs;
            auto&       draw_count       = m_desc.draw_count;
            auto&       index_buf        = m_desc.index_buf;
            auto&       force_dyn_upload = m_desc.force_dyn_upload;
            update_dyn_buf_op                = [&mesh,
                                                &vertex_bufs,
                                                &draw_count,
                                                &index_buf,
                                                dyn_buf,
                                                &force_dyn_upload]() {
                const bool dirty        = mesh.Dirty().load();
                const bool needs_upload = dirty || force_dyn_upload;
                if (needs_upload) {
                    auto ensure_vertex_subref = [&](usize                              array_index,
                                                    const wallpaper::SceneVertexArray& vertex) {
                        if (vertex_bufs.size() <= array_index) {
                            vertex_bufs.resize(array_index + 1);
                        }

                        auto&      buf                = vertex_bufs[array_index];
                        const auto required_live_size = static_cast<VkDeviceSize>(
                            std::max<usize>(vertex.DataSizeOf(), vertex.OneSizeOf()));
                        if (buf && buf.size >= required_live_size) return true;

                        // Dynamic custom-shader meshes may grow after a pass was prepared. Keep
                        // this as a renderer-level buffer refresh mechanism for authored
                        // dynamic meshes, while first-class text is handled by TextPass and
                        // never enters CustomShaderPass as glyph helper nodes.
                        const auto required_size = GrowDynamicSuballocationSize(
                            buf ? buf.size : 0,
                            required_live_size,
                            static_cast<VkDeviceSize>(vertex.CapacitySizeOf()),
                            static_cast<VkDeviceSize>(vertex.OneSizeOf()));
                        if (required_size < required_live_size) {
                            LOG_ERROR("DynamicVertexUpload: live data exceeds capacity node='%s' "
                                      "live=%zu capacity=%zu",
                                      mesh.Material() != nullptr ? mesh.Material()->name.c_str()
                                                                 : "<unknown>",
                                      static_cast<size_t>(required_live_size),
                                      static_cast<size_t>(vertex.CapacitySizeOf()));
                            return false;
                        }
                        if (buf) {
                            dyn_buf->unallocateSubRef(buf);
                            buf = {};
                        }
                        if (! dyn_buf->allocateSubRef(required_size, buf)) {
                            return false;
                        }
                        force_dyn_upload = true;
                        return true;
                    };

                    auto release_unused_vertex_subrefs = [&]() {
                        while (vertex_bufs.size() > mesh.VertexCount()) {
                            auto& stale_buf = vertex_bufs.back();
                            if (stale_buf) dyn_buf->unallocateSubRef(stale_buf);
                            vertex_bufs.pop_back();
                        }
                    };

                    auto ensure_index_subref = [&](const wallpaper::SceneIndexArray& indice) {
                        const auto required_live_size = static_cast<VkDeviceSize>(
                            std::max<usize>(indice.DataSizeOf(), kDynamicIndexQuadFloorSize));
                        if (index_buf && index_buf.size >= required_live_size) return true;

                        const auto required_size = GrowDynamicSuballocationSize(
                            index_buf ? index_buf.size : 0,
                            required_live_size,
                            static_cast<VkDeviceSize>(indice.CapacitySizeof()),
                            static_cast<VkDeviceSize>(sizeof(uint32_t) * 6));
                        if (required_size < required_live_size) {
                            LOG_ERROR("DynamicIndexUpload: live data exceeds capacity live=%zu "
                                      "capacity=%zu",
                                      static_cast<size_t>(required_live_size),
                                      static_cast<size_t>(indice.CapacitySizeof()));
                            return false;
                        }
                        if (index_buf) {
                            dyn_buf->unallocateSubRef(index_buf);
                            index_buf = {};
                        }
                        if (! dyn_buf->allocateSubRef(required_size, index_buf)) {
                            return false;
                        }
                        force_dyn_upload = true;
                        return true;
                    };

                    release_unused_vertex_subrefs();
                    for (usize i = 0; i < mesh.VertexCount(); i++) {
                        const auto& vertex = mesh.GetVertexArray(i);
                        if (! ensure_vertex_subref(i, vertex)) {
                            mesh.SetDirty();
                            return;
                        }
                        auto& buf = vertex_bufs[i];
                        if (! dyn_buf->writeToBuf(
                                buf, { (uint8_t*)vertex.Data(), vertex.DataSizeOf() })) {
                            mesh.SetDirty();
                            return;
                        }
                    }
                    if (mesh.IndexCount() > 0) {
                        auto& indice = mesh.GetIndexArray(0);
                        if (! ensure_index_subref(indice)) {
                            mesh.SetDirty();
                            return;
                        }
                        u32 count  = (u32)((indice.RenderDataCount() * 2) / 3);
                        draw_count = count * 3;
                        auto& buf  = index_buf;
                        if (! dyn_buf->writeToBuf(
                                buf, { (uint8_t*)indice.Data(), indice.DataSizeOf() })) {
                            mesh.SetDirty();
                            return;
                        }
                    } else {
                        // Dynamic non-indexed meshes are still drawable. Text effect outputs
                        // use a four-vertex triangle-strip card that is resized in place when
                        // Date/Day/ Clock content changes; clearing draw_count here made the
                        // bridge and effect passes execute successfully while submitting no
                        // final composite geometry at all. The first vertex binding defines the
                        // vertex count for non-indexed draws, matching the static prepare
                        // path's draw contract.
                        draw_count = mesh.VertexCount() > 0
                                         ? static_cast<u32>(mesh.GetVertexArray(0).DataSize() /
                                                            mesh.GetVertexArray(0).OneSize())
                                         : 0;
                        if (index_buf) {
                            dyn_buf->unallocateSubRef(index_buf);
                            index_buf = {};
                        }
                    }
                    // Clearing the pass-local bootstrap flag only after all writes succeed
                    // keeps a freshly compiled dynamic pass from getting stuck with empty GPU
                    // buffers if an earlier upload attempt bails out partway through due to an
                    // allocation/write failure. Subsequent frames will keep retrying until the
                    // first complete upload lands in the new subranges.
                    mesh.Dirty().store(false);
                    force_dyn_upload = false;
                }
            };
        }

        auto  block  = ref.blocks.front();
        auto* buf    = rr.dyn_buf;
        auto* bufref = &m_desc.ubo_buf;

        auto* node           = m_desc.node;
        auto* shader_updater = scene.shaderValueUpdater.get();
        auto& sprites        = m_desc.sprites_map;
        auto& vk_textures    = m_desc.vk_textures;
        // The update lambda still needs the live material pointer to write authored uniforms after
        // the one-off audio diagnostics are removed; keep that dependency explicit in the capture
        // list instead of rediscovering the material through the scene node at execution time.
        auto* material = mesh.Material();

        // Keep Star-River-style dynamic mesh uploads separate from general pass updates. Only the
        // vertex/index bytes need to move before m_dyn_buf->recordUpload(); uniform and sprite
        // updates stay in execute() so Date/Clock effect composites keep their original layout
        // timing and do not shift when the upload fix is active.
        m_desc.update_dynamic_mesh_op = update_dyn_buf_op;
        m_desc.update_op =
            [shader_updater, block, buf, bufref, node, material, &sprites, &vk_textures,
             camera_override = m_desc.camera_override,
             use_active_camera_for_uniforms = m_desc.use_active_camera_for_uniforms,
             use_active_camera_for_parallax = m_desc.use_active_camera_for_parallax]() {
                auto update_unf_op = [&block, buf, bufref](std::string_view       name,
                                                           wallpaper::ShaderValue value) {
                    UpdateUniform(buf, *bufref, block, name, value);
                };
                if (material != nullptr) {
                    WriteMaterialUniforms(buf, *bufref, block, *material);
                }
                const ShaderUniformOverrides overrides {
                    .camera_name = camera_override,
                    .use_camera_override = !camera_override.empty(),
                    .use_active_camera_for_uniforms = use_active_camera_for_uniforms,
                    .use_active_camera_for_parallax = use_active_camera_for_parallax,
                };
                shader_updater->UpdateUniforms(
                    node,
                    sprites,
                    update_unf_op,
                    (overrides.use_camera_override ||
                     overrides.use_active_camera_for_uniforms)
                        ? &overrides
                        : nullptr);
                // update image slot for sprites
                {
                    for (auto& [i, sp] : sprites) {
                        if (i >= vk_textures.size()) continue;
                        vk_textures.at(i).active = sp.GetCurFrame().imageId;
                    }
                }
            };

        auto exists_unf_op = [&block](std::string_view name) {
            return exists(block.member_map, name);
        };
        shader_updater->InitUniforms(node, exists_unf_op);

        // memset uniform buf
        buf->fillBuf(*bufref, 0, bufref->size, 0);
        WriteMaterialUniforms(buf, *bufref, block, *mesh.Material());
        m_desc.update_op();
        if (m_desc.update_dynamic_mesh_op) m_desc.update_dynamic_mesh_op();
    }

    {
        m_desc.clear_value =
            BuildCustomShaderClearValue(scene, *mesh.Material(), m_desc.clear_before_draw);
    }
    setPrepared();
}

void CustomShaderPass::prepareDeferred(Scene& scene, const Device& device, RenderingResources& rr) {
    if (requestDeferredPrepareResources(scene, device) == DeferredPrepareResourcesState::Waiting) {
        return;
    }
    if (! StaticSceneTexturesResidentForDeferredPrepare(scene, device, m_desc)) {
        return;
    }
    prepare(scene, device, rr);
}

DeferredPrepareResourcesState
CustomShaderPass::requestDeferredPrepareResources(Scene& scene, const Device& device) {
    constexpr std::size_t kDeferredStaticTextureStageBudgetBytes = 64u * 1024u * 1024u;
    bool                  waiting                                = false;

    for (usize texture_index = 0; texture_index < m_desc.textures.size(); texture_index++) {
        const auto& tex_name = m_desc.textures[texture_index];
        if (tex_name.empty()) continue;
        if (scene.renderTargets.count(tex_name) != 0 || wallpaper::IsSpecTex(tex_name)) continue;
        if (scene.dirtyImportedTextureKeys.count(tex_name) != 0) continue;
        if (device.tex_cache().FindTex(tex_name).has_value()) continue;

        const auto pending_streaming_state = device.tex_cache().StagePendingTexUploads(
            tex_name, kDeferredStaticTextureStageBudgetBytes);
        if (pending_streaming_state == TextureCacheStreamingState::Waiting) {
            waiting = true;
            continue;
        }
        if (pending_streaming_state == TextureCacheStreamingState::Ready &&
            device.tex_cache().FindTex(tex_name).has_value()) {
            continue;
        }

        const auto texture_it = scene.textures.find(tex_name);
        if (texture_it == scene.textures.end() || texture_it->second.isVideo) continue;

        // Deferred visibility prepare follows the same split as modern streaming renderers:
        // expensive disk/decompression work is requested from the scene asset cache first, and the
        // render thread only builds Vulkan residency after those CPU bytes are ready. This keeps a
        // newly visible deferred layer from blocking the whole frame on WPTexImageParser::Parse().
        const auto request = scene.RequestParsedImageAsync(tex_name);
        switch (request.state) {
        case Scene::ParsedImageRequestState::Ready:
            if (request.image != nullptr) {
                std::optional<usize> priority_slot;
                if (const auto sprite_it = m_desc.sprites_map.find(texture_index);
                    sprite_it != m_desc.sprites_map.end()) {
                    const auto image_id = sprite_it->second.GetCurFrame().imageId;
                    if (image_id >= 0) priority_slot = static_cast<usize>(image_id);
                }

                const auto streaming_state = device.tex_cache().StageTexUploads(
                    request.image, priority_slot, kDeferredStaticTextureStageBudgetBytes);
                scene.DropParsedImageCache(tex_name);
                if (streaming_state == TextureCacheStreamingState::Waiting) {
                    waiting = true;
                } else if (streaming_state == TextureCacheStreamingState::Failed) {
                    LOG_ERROR("CustomShaderPassDeferredResources: staging failed node='%s' "
                              "texture='%s'",
                              m_desc.node != nullptr ? m_desc.node->Name().c_str() : "<null>",
                              tex_name.c_str());
                }
            }
            break;
        case Scene::ParsedImageRequestState::Pending: waiting = true; break;
        case Scene::ParsedImageRequestState::Failed:
            LOG_ERROR("CustomShaderPassDeferredResources: parse failed node='%s' texture='%s'",
                      m_desc.node != nullptr ? m_desc.node->Name().c_str() : "<null>",
                      tex_name.c_str());
            break;
        }
    }

    return waiting ? DeferredPrepareResourcesState::Waiting : DeferredPrepareResourcesState::Ready;
}

bool CustomShaderPass::warmupPipeline(Scene& scene, const Device& device, RenderingResources& rr) {
    m_desc.scene = &scene;
    if (m_desc.node == nullptr || m_desc.node->Mesh() == nullptr ||
        m_desc.node->Mesh()->Material() == nullptr) {
        return false;
    }

    SceneMesh& mesh = *(m_desc.node->Mesh());

    std::vector<Uni_ShaderSpv> spvs;
    DescriptorSetInfo          descriptor_info;
    ShaderReflected            ref;
    {
        SceneShader& shader = *(mesh.Material()->customShader.shader);
        if (! GenReflect(shader.codes, spvs, ref)) {
            LOG_ERROR("pipeline warmup reflect failed, %s", shader.name.c_str());
            return false;
        }

        auto& bindings = descriptor_info.bindings;
        bindings.resize(ref.binding_map.size());
        std::transform(
            ref.binding_map.begin(), ref.binding_map.end(), bindings.begin(), [](auto& item) {
                return item.second;
            });
        PopulateTextureBindingsFromReflection(m_desc, ref, mesh.Material()->textures.size());
    }

    std::vector<VkVertexInputBindingDescription>   bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
    for (uint i = 0; i < mesh.VertexCount(); i++) {
        const auto& vertex    = mesh.GetVertexArray(i);
        auto        attrs_map = vertex.GetAttrOffsetMap();

        bind_descriptions.push_back(VkVertexInputBindingDescription {
            .binding   = i,
            .stride    = static_cast<uint32_t>(vertex.OneSizeOf()),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        });

        for (auto& item : ref.input_location_map) {
            const auto& name     = item.first;
            const auto& input    = item.second;
            const bool  has_attr = exists(attrs_map, name);
            const auto  offset   = has_attr ? attrs_map[name].offset : 0;
            attr_descriptions.push_back(VkVertexInputAttributeDescription {
                .location = input.location,
                .binding  = i,
                .format   = input.format,
                .offset   = static_cast<uint32_t>(offset),
            });
        }
    }
    auto render_state = BuildCustomShaderRenderState(*mesh.Material(), m_desc);

    auto opt = CreateRenderPass(device.handle(),
                                VK_FORMAT_R8G8B8A8_UNORM,
                                render_state.color_load_op,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                m_desc.model_pass,
                                m_desc.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                   : VK_ATTACHMENT_LOAD_OP_LOAD);
    if (! opt.has_value()) return false;
    auto& pass = opt.value();

    descriptor_info.push_descriptor = true;
    GraphicsPipeline pipeline;
    pipeline.toDefault();
    ApplyModelPipelineState(*mesh.Material(), m_desc, pipeline);
    m_desc.pipeline.debug_name =
        "CustomShaderPassWarmup[node=" +
        (m_desc.node != nullptr ? m_desc.node->Name() : std::string("(null)")) +
        ",output=" + m_desc.output + "]";
    m_desc.pipeline.cache_key = CustomShaderPipelineCompatibilityKey(
        render_state.color_load_op,
        m_desc.model_pass,
        m_desc.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
    pipeline.addDescriptorSetInfo(spanone { descriptor_info })
        .setColorBlendStates(spanone { render_state.color_blend })
        .setTopology(mesh.IndexCount() > 0 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                           : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
        .addInputBindingDescription(bind_descriptions)
        .addInputAttributeDescription(attr_descriptions);
    for (auto& spv : spvs) pipeline.addStage(std::move(spv));

    return pipeline.create(device, pass, m_desc.pipeline, rr.pipeline_cache.get());
}

void CustomShaderPass::refreshResources(Scene& scene, const Device& device,
                                        RenderingResources& rr) {
    // Resource refreshes reuse the pass object. Refresh the scene pointer first so dependency
    // checks observe the current render-target table and text bridge state without logging.
    m_desc.scene = &scene;
    // Resource-only refreshes intentionally keep the compiled shader pipeline, reflected bindings,
    // and uploaded mesh/UBO allocations intact. The expensive part that changes for effect-backed
    // minute updates is the texture-cache-backed image handle set and the framebuffer that wraps
    // the resized render target. Rebinding only those pieces avoids recompiling every shader pass
    // in the scene when the clock/date text changes shape.
    if (m_desc.node != nullptr && m_desc.node->Mesh() != nullptr) {
        auto& mesh = *m_desc.node->Mesh();
        if (! mesh.Dynamic() && mesh.Dirty().load()) {
            // Resource-only refreshes were originally written for effects whose geometry never
            // changes after graph build. Refactored text effects break that assumption: runtime
            // updates now mutate the static blur/compose quads of already-compiled effect passes.
            // If we only recreate textures/framebuffers here, the pass keeps drawing the old GPU
            // vertex buffer even though the SceneMesh carries the new map-rate-adjusted quad. By
            // dropping back to the normal prepare path for dirty static meshes we force the pass
            // to re-upload its mesh data and make runtime text-effect geometry changes actually
            // visible on screen.
            destory(device, rr);
            return;
        }
    }

    const auto output_target_it = scene.renderTargets.find(m_desc.output);
    if (output_target_it == scene.renderTargets.end()) {
        LOG_ERROR(
            "CustomShaderPassRefresh: output target not found before refresh node='%s' output='%s'",
            m_desc.node != nullptr ? m_desc.node->Name().c_str() : "<null>",
            m_desc.output.c_str());
        setPrepared(false);
        return;
    }
    const auto previous_output_view   = m_desc.vk_output.view;
    const auto previous_output_extent = m_desc.vk_output.extent;
    const auto desired_output_key     = wallpaper::vulkan::ToTexKey(output_target_it->second);
    const bool output_extent_changed =
        previous_output_extent.width != static_cast<uint32_t>(desired_output_key.width) ||
        previous_output_extent.height != static_cast<uint32_t>(desired_output_key.height);
    if (output_extent_changed) {
        // Only an output size change requires releasing the framebuffer before TextureCache::Query.
        // Recreating every framebuffer on every resource refresh was both wasteful and unsafe for
        // unchanged `_rt_default` passes: those passes can keep their attachment while only their
        // sampled input texture changes, exactly like a particle pass consuming a live source.
        m_desc.fb.reset();
    }
    if (! RefreshCustomShaderPassTextures(scene, device, m_desc)) {
        LOG_ERROR("CustomShaderPassRefresh: texture refresh failed node='%s' output='%s'",
                  m_desc.node != nullptr ? m_desc.node->Name().c_str() : "<null>",
                  m_desc.output.c_str());
        setPrepared(false);
        return;
    }
    const bool output_view_changed = previous_output_view != m_desc.vk_output.view;
    const bool framebuffer_missing = ! m_desc.fb;
    if (framebuffer_missing || output_extent_changed || output_view_changed) {
        if (! RecreateCustomShaderPassFramebuffer(device, rr, m_desc)) {
            setPrepared(false);
            return;
        }
    }
    if (m_desc.dyn_vertex && m_desc.update_dynamic_mesh_op != nullptr && m_desc.node != nullptr &&
        m_desc.node->Mesh() != nullptr &&
        (m_desc.force_dyn_upload || m_desc.node->Mesh()->Dirty().load())) {
        // Text-backed effect passes keep their render-graph topology stable while the final
        // source quad changes size. Uploading the dirty dynamic mesh during the resource-refresh
        // phase lets the compile-time dynamic-buffer copy include the new quad before the first
        // post-refresh draw, instead of binding a fresh suballocation that still contains old data.
        m_desc.update_dynamic_mesh_op();
    }
}

void CustomShaderPass::updateBeforeUpload() {
    if (! m_desc.update_dynamic_mesh_op) return;

    if (m_desc.should_execute && ! m_desc.should_execute()) {
        return;
    }

    if (m_desc.node != nullptr && ! m_desc.node->LocalVisible()) {
        return;
    }

    const bool node_visible = m_desc.node == nullptr ? true : m_desc.node->Visible();
    if (m_desc.node != nullptr && ! node_visible && ! m_desc.execute_when_hidden) {
        return;
    }

    // VulkanRender records the shared dynamic-buffer upload before pass execution. Updating only
    // dynamic vertex/index bytes here preserves the original execute-time uniform ordering for
    // text/effect composites while still preventing reused-source particle fields, such as
    // 3308867900's Star River layer, from drawing from stale or never-uploaded subranges.
    m_desc.update_dynamic_mesh_op();
}

void CustomShaderPass::execute(const Device& device, RenderingResources& rr) {
    if (m_desc.should_execute && ! m_desc.should_execute()) {
        // Runtime-gated helper passes stay in the render graph so visibility flips do not rebuild
        // framebuffer topology. Returning before uniform updates and draw submission makes the pass
        // a true no-op on frames where its fallback branch is not active.
        if (WallpaperDebugLayerEnabled(m_desc.layer_id)) {
            LOG_INFO("DebugSpectrumPassSkip: layer=%d node='%s' output='%s' reason='should_execute'",
                     m_desc.layer_id,
                     m_desc.node != nullptr ? m_desc.node->Name().c_str() : "",
                     m_desc.output.c_str());
        }
        releaseFinalReadTexs(device);
        return;
    }

    if (m_desc.node != nullptr && ! m_desc.node->LocalVisible()) {
        // execute_when_hidden is only for layer-level invisibility, such as offscreen dependency
        // sources that must keep rendering while their authored layer is hidden in the main scene.
        // Effect-local visibility is a stricter contract: a hidden effect must not run its shader
        // pass, otherwise the hidden branch would still overwrite the ping-pong output that the
        // bypass copy is responsible for preserving.
        if (WallpaperDebugLayerEnabled(m_desc.layer_id)) {
            LOG_INFO("DebugSpectrumPassSkip: layer=%d node='%s' output='%s' reason='local_visible'",
                     m_desc.layer_id,
                     m_desc.node != nullptr ? m_desc.node->Name().c_str() : "",
                     m_desc.output.c_str());
        }
        releaseFinalReadTexs(device);
        return;
    }

    const bool node_visible = m_desc.node == nullptr ? true : m_desc.node->Visible();
    if (m_desc.node != nullptr && ! node_visible && ! m_desc.execute_when_hidden) {
        // The render graph has still reached this pass's ordering point even when authored
        // visibility turns the shader into a no-op for the frame. Releasing final-read keys here
        // prevents temporary render targets from staying pinned only because no draw was recorded.
        if (WallpaperDebugLayerEnabled(m_desc.layer_id)) {
            LOG_INFO("DebugSpectrumPassSkip: layer=%d node='%s' output='%s' reason='visible' "
                     "execute-when-hidden=%s",
                     m_desc.layer_id,
                     m_desc.node != nullptr ? m_desc.node->Name().c_str() : "",
                     m_desc.output.c_str(),
                     m_desc.execute_when_hidden ? "true" : "false");
        }
        releaseFinalReadTexs(device);
        return;
    }

    if (WallpaperDebugLayerEnabled(m_desc.layer_id)) {
        LOG_INFO("DebugSpectrumPassExecute: layer=%d node='%s' output='%s' execute-when-hidden=%s",
                 m_desc.layer_id,
                 m_desc.node != nullptr ? m_desc.node->Name().c_str() : "",
                 m_desc.output.c_str(),
                 m_desc.execute_when_hidden ? "true" : "false");
    }

    if (m_desc.update_op) m_desc.update_op();

    auto&                   cmd    = rr.command;
    auto&                   outext = m_desc.vk_output.extent;
    VkImageSubresourceRange base_srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_ARRAY_LAYERS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_MIP_LEVELS,
    };
    for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
        auto& slot    = m_desc.vk_textures[i];
        int   binding = m_desc.vk_tex_binding[i];
        if (binding < 0) continue;
        if (slot.slots.empty()) continue;
        auto&                 img = slot.getActive();
        VkDescriptorImageInfo desc_img { img.sampler,
                                         img.view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet  wset {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext           = nullptr,
            .dstSet          = {},
            .dstBinding      = (uint32_t)binding,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &desc_img,
        };
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wset);

        VkImageMemoryBarrier imb {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            // Render-target inputs are often produced by the immediately previous pass in the
            // same command buffer. That producer writes through the color-attachment path, not the
            // fragment-shader read path, so the consumer must wait on color attachment writes
            // before sampling. Without this, first-class text bridges can be drawn correctly by
            // TextPass and still appear missing when their effect/composite pass samples the
            // freshly written offscreen image.
            .srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = img.handle,
            .subresourceRange = base_srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            imb);
    }

    if (m_desc.ubo_buf) {
        VkDescriptorBufferInfo desc_buf {
            rr.dyn_buf->gpuBuf(),
            m_desc.ubo_buf.offset,
            m_desc.ubo_buf.size,
        };
        VkWriteDescriptorSet wset {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext           = nullptr,
            .dstSet          = {},
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &desc_buf,
        };
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wset);
    }

    m_desc.depth_clear_value.depthStencil = { 1.0f, 0 };
    std::array<VkClearValue, 2> clear_values { m_desc.clear_value, m_desc.depth_clear_value };
    VkRenderPassBeginInfo       pass_begin_info {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext       = nullptr,
        .renderPass  = *m_desc.pipeline.pass,
        .framebuffer = *m_desc.fb,
        .renderArea =
            VkRect2D {
                .offset = { 0, 0 },
                .extent = { outext.width, outext.height },
            },
        .clearValueCount = m_desc.model_pass ? 2u : 1u,
        .pClearValues    = clear_values.data(),
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

    auto gpu_buf = m_desc.dyn_vertex ? rr.dyn_buf->gpuBuf() : rr.vertex_buf->gpuBuf();

    for (usize i = 0; i < m_desc.vertex_bufs.size(); i++) {
        auto& buf = m_desc.vertex_bufs[i];
        cmd.BindVertexBuffers((u32)i, 1, &gpu_buf, &buf.offset);
    }
    if (m_desc.index_buf) {
        cmd.BindIndexBuffer(gpu_buf, m_desc.index_buf.offset, VK_INDEX_TYPE_UINT16);
        cmd.DrawIndexed(m_desc.draw_count, 1, 0, 0, 0);
    } else {
        cmd.Draw(m_desc.draw_count, 1, 0, 0);
    }

    cmd.EndRenderPass();
    // Temporary render targets may only be returned to TextureCache after the pass has actually
    // consumed them in the recorded frame. Releasing during prepare/resource-refresh is unsafe:
    // all passes are prepared before any pass executes, so a later pass can accidentally bind a
    // same-sized but unrelated physical image for a still-live logical key.
    releaseFinalReadTexs(device);
}

void CustomShaderPass::destory(const Device&, RenderingResources& rr) {
    m_desc.update_op              = {};
    m_desc.update_dynamic_mesh_op = {};
    // Retiring a hidden layer must drop framebuffer/image/buffer residency while leaving cached
    // PSO ownership to GraphicsPipelineStateCache. This mirrors game-engine visibility handling:
    // textures and render targets can be evicted, but the immutable shader pipeline remains warm
    // for the next show transition instead of recompiling on the visible frame.
    m_desc.fb.reset();
    m_desc.vk_textures.clear();
    m_desc.vk_tex_binding.clear();
    m_desc.vk_output       = {};
    m_desc.depth_image_ref = nullptr;
    {
        auto& buf = m_desc.dyn_vertex ? rr.dyn_buf : rr.vertex_buf;
        for (auto& bufref : m_desc.vertex_bufs) {
            buf->unallocateSubRef(bufref);
        }
        m_desc.vertex_bufs.clear();
    }
    if (m_desc.index_buf) {
        auto& buf = m_desc.dyn_vertex ? rr.dyn_buf : rr.vertex_buf;
        buf->unallocateSubRef(m_desc.index_buf);
        m_desc.index_buf = {};
    }
    rr.dyn_buf->unallocateSubRef(m_desc.ubo_buf);
    m_desc.ubo_buf = {};
    // Resource-only render-graph refreshes keep the pass objects alive inside the graph, so the
    // only signal that forces a new Vulkan preparation round is the prepared flag. Leaving it set
    // after we release buffer suballocations lets the next frame reuse stale descriptor/image
    // bindings that still point at destroyed cache entries, which is exactly the kind of
    // use-after-destroy that can crash inside vkCmdPushDescriptorSetKHR on the render thread.
    setPrepared(false);
}

void CustomShaderPass::setDescTex(u32 index, std::string_view tex_key) {
    assert(index < m_desc.textures.size());
    if (index >= m_desc.textures.size()) return;
    m_desc.textures[index] = tex_key;
}
