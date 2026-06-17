#include "SceneImageEffectLayer.h"
#include "SceneNode.h"

#include "SpecTexs.hpp"
#include "core/StringHelper.hpp"

using namespace wallpaper;

namespace
{
std::string ResolvePingPongAlias(std::string_view value, std::string_view ppong_a,
                                 std::string_view ppong_b) {
    if (sstart_with(value, WE_EFFECT_PPONG_PREFIX_A)) return std::string(ppong_a);
    if (sstart_with(value, WE_EFFECT_PPONG_PREFIX_B)) return std::string(ppong_b);
    if (value == SpecTex_Default) return std::string(ppong_b);
    return std::string(value);
}
} // namespace

SceneImageEffectLayer::SceneImageEffectLayer(SceneNode* node, float w, float h,
                                             std::string_view pingpong_a,
                                             std::string_view pingpong_b)
    : m_worldNode(node),
      m_pingpong_a(pingpong_a),
      m_pingpong_b(pingpong_b),
      m_final_mesh(std::make_unique<SceneMesh>()),
      m_final_node(std::make_unique<SceneNode>()) {};

void SceneImageEffectLayer::ResolveEffect(const SceneMesh& default_mesh,
                                          std::string_view effect_cam) {
    std::string_view ppong_a = m_pingpong_a, ppong_b = m_pingpong_b;
    auto             swap_pp = [&ppong_a, &ppong_b]() {
        std::swap(ppong_a, ppong_b);
    };
    auto default_node = SceneNode();

    SceneImageEffectNode* last_output { nullptr };
    SceneImageEffect*     last_effect { nullptr };
    for (auto& eff : m_effects) {
        eff->SetBypassTargets(std::string(ppong_a), std::string(ppong_b));
        eff->SetFinalBypassTarget({});
        for (auto& cmd : eff->commands) {
            cmd.src = ResolvePingPongAlias(cmd.authored_src, ppong_a, ppong_b);
            cmd.dst = ResolvePingPongAlias(cmd.authored_dst, ppong_a, ppong_b);
        }
        for (auto it = eff->nodes.begin(); it != eff->nodes.end(); it++) {
            it->output = ResolvePingPongAlias(it->authored_output, ppong_a, ppong_b);
            if (it->output == std::string(ppong_b)) {
                last_output = &(*it);
                last_effect = eff.get();
            }

            assert(it->sceneNode->HasMaterial());

            auto& material = *(it->sceneNode->Mesh()->Material());
            {
                material.blenmode = BlendMode::Normal;
                it->camera_override = std::string(effect_cam);
                it->clear_before_draw = true;
                it->force_alpha_write = true;
                it->premultiplied_source_blend = false;
                it->sceneNode->CopyTrans(default_node);
                it->sceneNode->Mesh()->ChangeMeshDataFrom(default_mesh);
            }

            auto& texs = material.textures;
            texs.clear();
            texs.reserve(it->authored_textures.size());
            for (const auto& tex : it->authored_textures) {
                texs.push_back(ResolvePingPongAlias(tex, ppong_a, ppong_b));
            }
        }
        swap_pp();
    }
    if (last_output != nullptr) {
        last_output->output = SpecTex_Default;
        if (last_effect != nullptr) last_effect->SetFinalBypassTarget(std::string(SpecTex_Default));
        auto& mesh          = *(last_output->sceneNode->Mesh());
        auto& material      = *mesh.Material();
        {
            material.blenmode = m_final_blend;
            last_output->premultiplied_source_blend = true;
            if (fullscreen) {
                last_output->camera_override = std::string(effect_cam);
                last_output->sceneNode->CopyTrans(default_node);
                mesh.ChangeMeshDataFrom(default_mesh);
            } else {
                last_output->camera_override.clear();
                last_output->sceneNode->CopyTrans(*m_final_node);
                mesh.ChangeMeshDataFrom(*m_final_mesh);
            }
            last_output->clear_before_draw = false;
            last_output->force_alpha_write = false;
        }
    }
}
