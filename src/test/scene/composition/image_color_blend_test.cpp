#include "backend/scene/internal/engine/WESceneRenderPlanBuilder.hpp"
#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/parser/effect/ColorBlend.hpp"
#include "backend/scene/internal/SpecTexs.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneCamera.h"
#include "backend/scene/internal/scene/include/scene/SceneImageEffectLayer.h"
#include "backend/scene/internal/wpscene/WPMaterial.h"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"
#include "render/vulkanrender/CustomShaderPass.hpp"
#include "render/vulkanrender/PassCommon.hpp"
#include "rendergraph/RenderGraph.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
namespace rg = wallpaper::rg;
namespace vk = wallpaper::vulkan;

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "image color blend test failure: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

class MemoryFs final : public wallpaper::fs::Fs {
public:
    explicit MemoryFs(std::unordered_map<std::string, std::string> files)
        : files_(std::move(files)) {}

    bool Contains(std::string_view path) const override {
        return files_.contains(std::string(path));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStream> Open(std::string_view path) override {
        const auto it = files_.find(std::string(path));
        if (it == files_.end()) return nullptr;
        return std::make_shared<wallpaper::fs::MemBinaryStream>(
            std::vector<uint8_t>(it->second.begin(), it->second.end()));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStreamW> OpenW(std::string_view) override {
        return nullptr;
    }

private:
    std::unordered_map<std::string, std::string> files_;
};

void MountAssets(wallpaper::fs::VFS& vfs) {
    const std::string vertex_shader = R"(
        attribute vec3 a_Position;
        attribute vec2 a_TexCoord;
        varying vec2 v_TexCoord;
        void main() {
            gl_Position = vec4(a_Position, 1.0);
            v_TexCoord = a_TexCoord;
        }
    )";
    const std::string image_fragment_shader = R"(
        // [COMBO] {"combo":"BLENDMODE","default":0}
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = vec4(v_TexCoord, 0.5, 0.75);
        }
    )";
    const std::string sample_fragment_shader = R"(
        // [COMBO] {"combo":"BLENDMODE","default":0}
        uniform sampler2D g_Texture0;
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = texture(g_Texture0, v_TexCoord);
        }
    )";
    const std::string blur_fragment_shader = R"(
        uniform sampler2D g_Texture0; // {"material":"previous"}
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = texture(g_Texture0, v_TexCoord);
        }
    )";

    Require(vfs.Mount(
                "/assets",
                std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
                    { "/image.json",
                      R"({
                          "width": 64,
                          "height": 64,
                          "material": "materials/image.json"
                      })" },
                    { "/materials/image.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "genericimage",
                                  "textures": [],
                                  "blending": "translucent"
                              }
                          ]
                      })" },
                    { "/effects/blur.json",
                      R"({
                          "name": "Blur",
                          "passes": [
                              { "material": "materials/blur.json" }
                          ]
                      })" },
                    { "/materials/blur.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "effects/blur",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/materials/util/effectpassthrough.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "passthrough",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/shaders/genericimage.vert", vertex_shader },
                    { "/shaders/genericimage.frag", image_fragment_shader },
                    { "/shaders/effects/blur.vert", vertex_shader },
                    { "/shaders/effects/blur.frag", blur_fragment_shader },
                    { "/shaders/passthrough.vert", vertex_shader },
                    { "/shaders/passthrough.frag", sample_fragment_shader },
                }),
                "image-color-blend-assets"),
            "failed to mount image color blend assets");
}

std::shared_ptr<wallpaper::Scene> ParseScene() {
    wallpaper::WPSceneParser parser;
    wallpaper::fs::VFS vfs;
    wallpaper::audio::SoundManager sound_manager;
    MountAssets(vfs);

    return parser.Parse("image-color-blend",
                        R"({
                            "camera": {
                                "center": [0, 0, 0],
                                "eye": [0, 0, 1],
                                "up": [0, 1, 0]
                            },
                            "general": {
                                "clearcolor": [0, 0, 0],
                                "orthogonalprojection": {
                                    "width": 128,
                                    "height": 64
                                },
                                "zoom": 1
                            },
                            "objects": [
                                {
                                    "id": 10,
                                    "name": "DirectBlend",
                                    "image": "image.json",
                                    "origin": [32, 32, 0],
                                    "angles": [0, 0, 0],
                                    "scale": [1, 1, 1],
                                    "colorBlendMode": 5,
                                    "visible": true
                                },
                                {
                                    "id": 20,
                                    "name": "EffectBlend",
                                    "image": "image.json",
                                    "origin": [96, 32, 0],
                                    "angles": [0, 0, 0],
                                    "scale": [1, 1, 1],
                                    "colorBlendMode": 7,
                                    "copybackground": true,
                                    "visible": true,
                                    "effects": [
                                        {
                                            "id": 21,
                                            "file": "effects/blur.json",
                                            "visible": true
                                        }
                                    ]
                                }
                            ]
                        })",
                        vfs,
                        sound_manager);
}

wallpaper::SceneImageEffectLayer* FindEffectLayer(wallpaper::Scene& scene, int32_t layer_id) {
    const auto nodes_it = scene.objectRuntimeNodes.find(layer_id);
    if (nodes_it == scene.objectRuntimeNodes.end()) return nullptr;
    for (auto* node : nodes_it->second) {
        if (node == nullptr || node->Camera().empty()) continue;
        const auto camera_it = scene.cameras.find(node->Camera());
        if (camera_it != scene.cameras.end() && camera_it->second != nullptr &&
            camera_it->second->HasImgEffect()) {
            return camera_it->second->GetImgEffect().get();
        }
    }
    return nullptr;
}

const vk::CustomShaderPass::Desc* FindPassByMaterial(rg::RenderGraph& graph,
                                                      std::string_view material_name) {
    for (const auto id : graph.topologicalOrder()) {
        const auto* pass = dynamic_cast<const vk::CustomShaderPass*>(graph.getPass(id));
        if (pass == nullptr || pass->desc().node == nullptr ||
            pass->desc().node->Mesh() == nullptr ||
            pass->desc().node->Mesh()->Material() == nullptr) {
            continue;
        }
        if (pass->desc().node->Mesh()->Material()->name == material_name) return &pass->desc();
    }
    return nullptr;
}
} // namespace

int main() {
    const auto direct_plan = wallpaper::ResolveImageColorBlendPlan(5, false, false);
    Require(direct_plan.apply_to_layer_material && !direct_plan.append_final_effect,
            "no-effect color blend must belong to the layer material");
    const auto effect_plan = wallpaper::ResolveImageColorBlendPlan(7, true, false);
    Require(!effect_plan.apply_to_layer_material && effect_plan.append_final_effect,
            "effect-backed color blend must append a final owner");
    const auto puppet_plan = wallpaper::ResolveImageColorBlendPlan(9, true, true);
    Require(puppet_plan.apply_to_layer_material && !puppet_plan.append_final_effect,
            "animated puppet color blend must belong to its layer-surface writer");

    wallpaper::wpscene::WPMaterial material;
    wallpaper::ApplyImageColorBlend(material, 12);
    Require(material.combos.count("BLENDMODE") == 1 && material.combos.at("BLENDMODE") == 12,
            "color blend combo was not applied to the chosen owner");
    wallpaper::ApplyImageEffectContext(material, true);
    Require(material.combos.count("COPYBG") == 1 && material.combos.at("COPYBG") == 1,
            "copybackground did not become an effect shader combo");

    VkPipelineColorBlendAttachmentState alpha_state {};
    vk::SetBlend(wallpaper::BlendMode::Translucent, alpha_state);
    vk::SetAlphaBlendWritePolicy(false, alpha_state);
    Require(alpha_state.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
                alpha_state.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            "alpha preservation changed RGB blending");
    Require(alpha_state.srcAlphaBlendFactor == VK_BLEND_FACTOR_ZERO &&
                alpha_state.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO,
            "alpha-preserving passes still modify destination alpha");

    auto scene = ParseScene();
    Require(scene != nullptr, "scene failed to parse");
    Require(FindEffectLayer(*scene, 10) == nullptr,
            "direct image color blend incorrectly created an effect bridge");

    auto* effect_layer = FindEffectLayer(*scene, 20);
    Require(effect_layer != nullptr, "effect-backed image has no effect bridge");
    Require(effect_layer->EffectCount() == 2,
            "effect-backed color blend did not append exactly one final effect");
    Require(effect_layer->GetEffect(1)->EffectName() ==
                "__hanabi_synthetic_color_blend_effect__",
            "color blend final owner has the wrong identity");
    Require(effect_layer->GetEffect(1)->nodes.size() == 1 &&
                effect_layer->GetEffect(1)->nodes.front().can_composite_final,
            "color blend final owner cannot composite the layer output");

    auto graph = wallpaper::BuildWESceneRenderPlan(*scene);
    Require(graph != nullptr, "render graph was not built");
    const auto* color_blend_pass = FindPassByMaterial(*graph, "passthrough");
    Require(color_blend_pass != nullptr &&
                color_blend_pass->output == wallpaper::SpecTex_Default,
            "synthetic color blend is not the visible final writer");
    Require(color_blend_pass->node != nullptr && color_blend_pass->node->Mesh() != nullptr &&
                color_blend_pass->node->Mesh()->Material() != nullptr &&
                color_blend_pass->node->Mesh()->Material()->blenmode !=
                    wallpaper::BlendMode::Additive,
            "copybackground color blend incorrectly uses a fixed additive material blend");

    return 0;
}
