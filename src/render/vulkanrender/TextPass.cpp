#include "TextPass.hpp"

#include "PassCommon.hpp"
#include "Resource.hpp"
#include "scene/SceneTextPrimitive.h"
#include "utils/Logging.h"

using namespace wallpaper::vulkan;

TextPass::TextPass(const Desc& desc): m_desc(desc) {}

TextPass::~TextPass() = default;

std::string TextPass::residencyKey() const {
    return "TextPass|node=" + std::to_string(reinterpret_cast<std::uintptr_t>(m_desc.node)) +
           "|layer=" + std::to_string(m_desc.layer_id) + "|output=" + m_desc.output;
}

bool TextPass::canReuseForResidency(const VulkanPass& next_pass) const {
    const auto* next = dynamic_cast<const TextPass*>(&next_pass);
    return next != nullptr && residencyKey() == next->residencyKey();
}

void TextPass::absorbResidencyGraphState(const VulkanPass& next_pass) {
    const auto* next = dynamic_cast<const TextPass*>(&next_pass);
    if (next == nullptr) return;
    m_desc.node = next->m_desc.node;
    m_desc.layer_id = next->m_desc.layer_id;
    m_desc.output = next->m_desc.output;
}

bool TextPass::referencesRenderTarget(std::string_view render_target) const {
    return m_desc.output == render_target;
}

bool TextPass::referencesTextLayer(int32_t layer_id) const {
    return layer_id != 0 && m_desc.layer_id == layer_id;
}

void TextPass::prepare(Scene& scene, const Device& device, RenderingResources&) {
    if (m_desc.node == nullptr || m_desc.layer_id == 0 || m_desc.output.empty()) {
        LOG_ERROR("TextPass: invalid text pass description");
        return;
    }
    if (scene.textPrimitives.count(m_desc.layer_id) == 0 ||
        scene.textPrimitives.at(m_desc.layer_id) == nullptr) {
        LOG_ERROR("TextPass: text primitive not found for layer %d", m_desc.layer_id);
        return;
    }
    if (scene.renderTargets.count(m_desc.output) == 0) {
        LOG_ERROR("TextPass: output render target not found: %s", m_desc.output.c_str());
        return;
    }

    const auto& rt = scene.renderTargets.at(m_desc.output);
    if (auto opt = device.tex_cache().Query(m_desc.output, ToTexKey(rt), ! rt.allowReuse);
        opt.has_value()) {
        m_desc.vk_output = opt.value();
    } else {
        LOG_ERROR("TextPass: query output image from cache failed: %s", m_desc.output.c_str());
        return;
    }

    setPrepared();
}

void TextPass::execute(const Device&, RenderingResources&) {
    if (!m_execution_diagnostic_emitted) {
        LOG_ERROR("TextPass: glyph atlas draw submission is not migrated yet for layer %d "
                  "output '%s'",
                  m_desc.layer_id,
                  m_desc.output.c_str());
        m_execution_diagnostic_emitted = true;
    }
    setPrepared(false);
}

void TextPass::destory(const Device&, RenderingResources&) {
    setPrepared(false);
    clearReleaseTexs();
}
