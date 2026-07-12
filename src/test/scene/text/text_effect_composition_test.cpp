#include "backend/scene/internal/SpecTexs.hpp"
#include "backend/scene/internal/engine/WESceneRenderPlanBuilder.hpp"
#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneCamera.h"
#include "backend/scene/internal/scene/include/scene/SceneImageEffectLayer.h"
#include "backend/scene/internal/text/WPTextLayer.hpp"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"
#include "render/vulkanrender/CustomShaderPass.hpp"
#include "render/vulkanrender/TextPass.hpp"
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
                 "text effect composition test failure: %.*s\n",
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
    const std::string sample_fragment_shader = R"(
        uniform sampler2D g_Texture0;
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = texture(g_Texture0, v_TexCoord);
        }
    )";
    const std::string background_fragment_shader = R"(
        uniform sampler2D g_Texture0;
        varying vec2 v_TexCoord;
        void main() {
            vec4 background = texture(g_Texture0, v_TexCoord);
            gl_FragColor = vec4(background.rgb, 0.0);
        }
    )";

    Require(vfs.Mount(
                "/assets",
                std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
                    { "/effects/text_effect.json",
                      R"({
                          "name": "Text Effect",
                          "passes": [
                              { "material": "materials/text_effect.json" }
                          ]
                      })" },
                    { "/materials/text_effect.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "text_effect",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/materials/util/composelayer_clearalpha.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "text_background_seed",
                                  "textures": ["_rt_FullFrameBuffer"]
                              }
                          ]
                      })" },
                    { "/materials/util/effectpassthrough.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "text_final_passthrough",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/shaders/text_effect.vert", vertex_shader },
                    { "/shaders/text_effect.frag", sample_fragment_shader },
                    { "/shaders/text_background_seed.vert", vertex_shader },
                    { "/shaders/text_background_seed.frag", background_fragment_shader },
                    { "/shaders/text_final_passthrough.vert", vertex_shader },
                    { "/shaders/text_final_passthrough.frag", sample_fragment_shader },
                }),
                "text-effect-assets"),
            "failed to mount text effect assets");
}

std::shared_ptr<wallpaper::Scene> ParseScene() {
    wallpaper::WPSceneParser parser;
    wallpaper::fs::VFS vfs;
    wallpaper::audio::SoundManager sound_manager;
    MountAssets(vfs);

    return parser.Parse("text-effect-composition",
                        R"({
                            "camera": {
                                "center": [0, 0, 0],
                                "eye": [0, 0, 1],
                                "up": [0, 1, 0]
                            },
                            "general": {
                                "clearcolor": [0.1, 0.2, 0.3],
                                "orthogonalprojection": {
                                    "width": 256,
                                    "height": 128
                                },
                                "zoom": 1
                            },
                            "objects": [
                                {
                                    "id": 30,
                                    "name": "EffectText",
                                    "text": "TEXT",
                                    "font": "sans",
                                    "pointsize": 32,
                                    "size": [160, 48],
                                    "origin": [128, 64, 0],
                                    "angles": [0, 0, 0],
                                    "scale": [1, 1, 1],
                                    "color": [0.25, 0.5, 0.75],
                                    "alpha": 0.625,
                                    "backgroundbrightness": 3.0,
                                    "effects": [
                                        {
                                            "id": 31,
                                            "file": "effects/text_effect.json",
                                            "visible": true
                                        }
                                    ]
                                },
                                {
                                    "id": 32,
                                    "name": "ClockWithoutEffects",
                                    "text": "12:34",
                                    "font": "sans",
                                    "pointsize": 32,
                                    "size": [160, 48],
                                    "origin": [128, 32, 0],
                                    "angles": [0, 0, 0],
                                    "scale": [1, 1, 1],
                                    "copybackground": true
                                }
                            ]
                        })",
                        vfs,
                        sound_manager);
}

wallpaper::SceneNode* FindTextSourceNode(wallpaper::Scene& scene, int32_t layer_id) {
    const auto it = scene.objectRuntimeNodes.find(layer_id);
    if (it == scene.objectRuntimeNodes.end()) return nullptr;
    for (auto* node : it->second) {
        if (node != nullptr && node->Text() != nullptr && !node->Camera().empty()) return node;
    }
    return nullptr;
}

size_t FindPassIndex(const rg::RenderGraph& graph, const rg::Pass* pass) {
    const auto order = graph.topologicalOrder();
    for (size_t index = 0; index < order.size(); index++) {
        if (graph.getPass(order[index]) == pass) return index;
    }
    return order.size();
}

const vk::CustomShaderPass* FindShaderPass(const rg::RenderGraph& graph,
                                           std::string_view       material_name) {
    for (const auto id : graph.topologicalOrder()) {
        const auto* pass = dynamic_cast<const vk::CustomShaderPass*>(graph.getPass(id));
        if (pass != nullptr && pass->desc().node != nullptr &&
            pass->desc().node->Mesh() != nullptr &&
            pass->desc().node->Mesh()->Material() != nullptr &&
            pass->desc().node->Mesh()->Material()->name == material_name) {
            return pass;
        }
    }
    return nullptr;
}

std::vector<const vk::TextPass*> FindTextPasses(const rg::RenderGraph& graph, int32_t layer_id) {
    std::vector<const vk::TextPass*> passes;
    for (const auto id : graph.topologicalOrder()) {
        const auto* pass = dynamic_cast<const vk::TextPass*>(graph.getPass(id));
        if (pass != nullptr && pass->desc().layer_id == layer_id) passes.push_back(pass);
    }
    return passes;
}
} // namespace

int main() {
    auto scene = ParseScene();
    Require(scene != nullptr, "scene failed to parse");

    auto* text_node = FindTextSourceNode(*scene, 30);
    Require(text_node != nullptr, "effect text has no detached source node");
    const auto camera_it = scene->cameras.find(text_node->Camera());
    Require(camera_it != scene->cameras.end() && camera_it->second != nullptr &&
                camera_it->second->HasImgEffect(),
            "effect text has no image-effect bridge");

    auto effect_layer = camera_it->second->GetImgEffect();
    Require(effect_layer != nullptr, "effect text bridge is null");
    Require(effect_layer->PrefillNodes().size() == 1,
            "effect text must have one framebuffer background prefill");
    Require(effect_layer->EffectCount() == 1, "authored text effect was not preserved");

    const auto& prefill = effect_layer->PrefillNodes().front();
    Require(prefill.sceneNode != nullptr && prefill.sceneNode->Mesh() != nullptr &&
                prefill.sceneNode->Mesh()->Material() != nullptr,
            "background prefill has no materialized shader node");
    Require(prefill.output == effect_layer->FirstTarget(),
            "background prefill does not write the text source target");
    Require(!prefill.sceneNode->Mesh()->Material()->textures.empty() &&
                prefill.sceneNode->Mesh()->Material()->textures.front() ==
                    wallpaper::SpecTex_Default,
            "background prefill does not sample the accumulated framebuffer");

    auto graph = wallpaper::BuildWESceneRenderPlan(*scene);
    Require(graph != nullptr, "render graph was not built");

    const auto* background_pass = FindShaderPass(*graph, "text_background_seed");
    const auto* effect_pass = FindShaderPass(*graph, "text_effect");
    const auto* final_pass = FindShaderPass(*graph, "text_final_passthrough");
    const auto text_passes = FindTextPasses(*graph, 30);
    Require(background_pass != nullptr, "background prefill pass is missing");
    Require(effect_pass != nullptr, "authored text effect pass is missing");
    Require(final_pass != nullptr, "text final composite pass is missing");
    Require(text_passes.size() == 1, "glyph seed must be emitted exactly once");
    Require(text_passes.front()->desc().output == effect_layer->FirstTarget(),
            "glyph seed does not write the private text source");
    Require(!text_passes.front()->desc().clear_output,
            "glyph seed clears and destroys its framebuffer background prefill");

    const auto direct_text_passes = FindTextPasses(*graph, 32);
    Require(direct_text_passes.size() == 1 &&
                direct_text_passes.front()->desc().output == wallpaper::SpecTex_Default,
            "copybackground text without effects must write directly to the scene target");

    const auto background_index = FindPassIndex(*graph, background_pass);
    const auto glyph_index = FindPassIndex(*graph, text_passes.front());
    const auto effect_index = FindPassIndex(*graph, effect_pass);
    const auto final_index = FindPassIndex(*graph, final_pass);
    Require(background_index < glyph_index && glyph_index < effect_index && effect_index < final_index,
            "text composition pass order is not prefill -> glyph -> effect -> final");

    const auto initial_source_width =
        scene->renderTargets.at(effect_layer->FirstTarget()).width;
    scene->vfs = std::make_unique<wallpaper::fs::VFS>();
    auto text_state_it = scene->textLayers.find(30);
    Require(text_state_it != scene->textLayers.end(), "runtime text state is missing");
    Require(wallpaper::ApplyTextLayerPropertyValue(
                text_state_it->second,
                "text",
                wallpaper::WPDynamicValue(std::string(
                    "A MUCH LONGER DYNAMIC TEXT VALUE THAT MUST GROW THE EFFECT SOURCE"))),
            "dynamic text property update failed");
    Require(wallpaper::RebuildTextLayerSceneLayout(*scene, 30),
            "dynamic text layout rebuild failed");
    const auto grown_source_width =
        scene->renderTargets.at(effect_layer->FirstTarget()).width;
    Require(grown_source_width > initial_source_width,
            "dynamic text did not grow its private effect source target");
    Require(scene->dirtyRenderTargetKeys.count(effect_layer->FirstTarget()) == 1,
            "dynamic text resize did not mark its source target dirty");
    Require(scene->renderGraphResourcesDirty,
            "dynamic text resize did not request targeted render resources refresh");

    auto resized_graph = wallpaper::BuildWESceneRenderPlan(*scene);
    Require(resized_graph != nullptr, "resized text render graph was not built");
    const auto resized_text_passes = FindTextPasses(*resized_graph, 30);
    Require(resized_text_passes.size() == 1 &&
                resized_text_passes.front()->desc().output == effect_layer->FirstTarget(),
            "dynamic text rebuild duplicated or rerouted the glyph seed");
    return 0;
}
