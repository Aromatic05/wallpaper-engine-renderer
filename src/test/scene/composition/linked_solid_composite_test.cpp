#include "backend/scene/internal/SpecTexs.hpp"
#include "backend/scene/internal/engine/WESceneRenderPlanBuilder.hpp"
#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneCamera.h"
#include "backend/scene/internal/scene/include/scene/SceneImageEffectLayer.h"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"
#include "render/vulkanrender/CopyPass.hpp"
#include "render/vulkanrender/CustomShaderPass.hpp"
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
                 "linked solid composite test failure: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
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
    const std::string source_fragment_shader = R"(
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = vec4(v_TexCoord, 0.0, 1.0);
        }
    )";
    const std::string sampling_fragment_shader = R"(
        uniform sampler2D g_Texture0;
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = texture(g_Texture0, v_TexCoord);
        }
    )";

    Require(vfs.Mount(
                "/assets",
                std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
                    { "/source.json",
                      R"({
                          "width": 64,
                          "height": 64,
                          "solidlayer": true,
                          "material": "materials/source.json"
                      })" },
                    { "/consumer.json",
                      R"({
                          "width": 64,
                          "height": 64,
                          "material": "materials/consumer.json"
                      })" },
                    { "/materials/source.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "linked_source",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/materials/consumer.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "linked_consumer",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/materials/util/effectpassthrough.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "linked_passthrough",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/shaders/linked_source.vert", vertex_shader },
                    { "/shaders/linked_source.frag", source_fragment_shader },
                    { "/shaders/linked_consumer.vert", vertex_shader },
                    { "/shaders/linked_consumer.frag", sampling_fragment_shader },
                    { "/shaders/linked_passthrough.vert", vertex_shader },
                    { "/shaders/linked_passthrough.frag", sampling_fragment_shader },
                }),
                "linked-solid-assets"),
            "failed to mount linked-solid assets");
}

std::shared_ptr<wallpaper::Scene> ParseScene(bool source_visible, bool color_blend) {
    wallpaper::WPSceneParser parser;
    wallpaper::fs::VFS vfs;
    wallpaper::audio::SoundManager sound_manager;
    MountAssets(vfs);

    const std::string scene_json = std::string(R"({
        "camera": {
            "center": [0, 0, 0],
            "eye": [0, 0, 1],
            "up": [0, 1, 0]
        },
        "general": {
            "clearcolor": [0, 0, 0],
            "orthogonalprojection": {
                "width": 64,
                "height": 64
            },
            "zoom": 1
        },
        "objects": [
            {
                "id": 10,
                "name": "LinkedSolid",
                "image": "source.json",
                "origin": [0, 0, 0],
                "angles": [0, 0, 0],
                "scale": [1, 1, 1],
                "visible": )")
        + (source_visible ? "true" : "false")
        + (color_blend ? R"(,
                "colorBlendMode": 9)" : "")
        + R"(
            },
            {
                "id": 20,
                "name": "Consumer",
                "image": "consumer.json",
                "origin": [0, 0, 0],
                "angles": [0, 0, 0],
                "scale": [1, 1, 1],
                "dependencies": [10],
                "instance": {
                    "textures": ["_rt_imageLayerComposite_10"]
                },
                "visible": true
            }
        ]
    })";

    return parser.Parse("linked-solid", scene_json, vfs, sound_manager);
}

wallpaper::SceneImageEffectLayer* FindSourceEffectLayer(wallpaper::Scene& scene) {
    const auto runtime_it = scene.objectRuntimeNodes.find(10);
    if (runtime_it == scene.objectRuntimeNodes.end()) return nullptr;
    for (auto* node : runtime_it->second) {
        if (node == nullptr || node->Camera().empty()) continue;
        const auto camera_it = scene.cameras.find(node->Camera());
        if (camera_it == scene.cameras.end() || camera_it->second == nullptr ||
            ! camera_it->second->HasImgEffect()) {
            continue;
        }
        return camera_it->second->GetImgEffect().get();
    }
    return nullptr;
}

const vk::CopyPass::Desc* FindLinkPublication(rg::RenderGraph& graph, int32_t layer_id) {
    const auto expected_dst = wallpaper::GenLinkTex(static_cast<wallpaper::idx>(layer_id));
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        if (auto* copy = dynamic_cast<vk::CopyPass*>(pass); copy != nullptr &&
            copy->desc().dst == expected_dst) {
            return &copy->desc();
        }
    }
    return nullptr;
}

const vk::CustomShaderPass::Desc* FindConsumerPass(rg::RenderGraph& graph) {
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        auto* shader = dynamic_cast<vk::CustomShaderPass*>(pass);
        if (shader == nullptr || shader->desc().layer_id != 20) continue;
        return &shader->desc();
    }
    return nullptr;
}

const vk::CustomShaderPass::Desc* FindVisibleSourcePublisher(rg::RenderGraph& graph) {
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        auto* shader = dynamic_cast<vk::CustomShaderPass*>(pass);
        if (shader == nullptr || shader->desc().layer_id != 10 ||
            shader->desc().output != wallpaper::SpecTex_Default) {
            continue;
        }
        if (shader->desc().should_execute) return &shader->desc();
    }
    return nullptr;
}

void Verify(bool source_visible, bool color_blend = false) {
    auto scene = ParseScene(source_visible, color_blend);
    Require(scene != nullptr, "linked-solid scene failed to parse");
    Require(scene->offscreenDependencyLayerIds.count(10) == 1,
            "linked solid source was not classified as an offscreen dependency");

    auto* effect_layer = FindSourceEffectLayer(*scene);
    Require(effect_layer != nullptr, "linked solid source has no effect bridge");
    Require(effect_layer->EffectCount() == (color_blend ? 2U : 1U),
            "linked solid source received the wrong number of final-owner effects");
    Require(effect_layer->GetEffect(0)->EffectName() == "__hanabi_linked_solid_passthrough",
            "linked solid source received the wrong synthetic effect");
    if (color_blend) {
        Require(effect_layer->GetEffect(1)->EffectName() ==
                    "__hanabi_synthetic_color_blend_effect__",
                "linked solid color blend did not move to the actual final owner");
    }
    const auto consumer_nodes_it = scene->objectRuntimeNodes.find(20);
    Require(consumer_nodes_it != scene->objectRuntimeNodes.end(),
            "linked solid consumer has no runtime node");
    const wallpaper::SceneMaterial* consumer_material = nullptr;
    for (auto* node : consumer_nodes_it->second) {
        if (node != nullptr && node->Mesh() != nullptr && node->Mesh()->Material() != nullptr) {
            consumer_material = node->Mesh()->Material();
            break;
        }
    }
    Require(consumer_material != nullptr, "linked solid consumer has no scene material");
    Require(! consumer_material->textures.empty() &&
                consumer_material->textures.front() == wallpaper::GenLinkTex(10),
            "instance texture override did not reach the consumer scene material");

    auto graph = wallpaper::BuildWESceneRenderPlan(*scene);
    Require(graph != nullptr, "linked-solid render graph was not built");

    const auto* publication = FindLinkPublication(*graph, 10);
    Require(publication != nullptr, "linked solid source did not publish a private link texture");
    Require(publication->src == effect_layer->ResolvedPrivateOutputTarget(),
            "link publication must copy the resolved private final output");
    Require(publication->src != wallpaper::SpecTex_Default,
            "link publication must not sample the cumulative default framebuffer");

    const auto* consumer = FindConsumerPass(*graph);
    Require(consumer != nullptr, "linked solid consumer pass is missing");
    Require(! consumer->textures.empty() &&
                consumer->textures.front() == wallpaper::GenLinkTex(10),
            "consumer did not bind the explicit linked-layer publication");

    const auto* publisher = FindVisibleSourcePublisher(*graph);
    Require(publisher != nullptr,
            "linked solid source has no gated visible final composite publisher");
    Require(static_cast<bool>(publisher->should_execute),
            "linked solid visible publisher has no runtime gate");
    Require(publisher->should_execute() == source_visible,
            "linked solid visible publisher gate does not follow layer visibility");
}
} // namespace

int main() {
    Verify(true);
    Verify(false);
    Verify(true, true);
    return 0;
}
