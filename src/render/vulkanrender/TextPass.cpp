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
    // The pass is intentionally render-graph visible before full text raster draw submission lands.
    // Later migration steps will bind text meshes/atlas textures here without changing graph shape.
}

void TextPass::destory(const Device&, RenderingResources&) {
    setPrepared(false);
    clearReleaseTexs();
}
