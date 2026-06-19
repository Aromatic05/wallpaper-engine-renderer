#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/parser/WPShaderParser.hpp"
#include "backend/scene/internal/scenescript/WPSceneScriptHost.hpp"
#include "backend/scene/internal/text/WPTextLayer.hpp"
#include "backend/scene/internal/SpecTexs.hpp"
#include "backend/scene/internal/engine/WESceneRenderPlanBuilder.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneMaterial.h"
#include "backend/scene/internal/scene/include/scene/SceneMesh.h"
#include "backend/scene/internal/scene/include/scene/SceneNode.h"
#include "backend/scene/internal/scene/include/scene/SceneTextPrimitive.h"
#include "backend/scene/internal/scene/include/scene/SceneTexture.h"
#include "backend/scene/internal/wpscene/WPMaterial.h"
#include "backend/scene/internal/wpscene/WPTextObject.h"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"
#include "render/vulkanrender/ClearPass.hpp"
#include "render/vulkanrender/TextPass.hpp"
#include "rendergraph/RenderGraph.hpp"

#include <cassert>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{

bool NearlyEqual(double lhs, double rhs) {
    return std::abs(lhs - rhs) <= 0.0001;
}

class MemoryFs final : public wallpaper::fs::Fs {
public:
    explicit MemoryFs(std::unordered_map<std::string, std::string> files)
        : m_files(std::move(files)) {}

    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) != 0;
    }

    std::shared_ptr<wallpaper::fs::IBinaryStream> Open(std::string_view path) override {
        const auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        return std::make_shared<wallpaper::fs::MemBinaryStream>(
            std::vector<uint8_t>(it->second.begin(), it->second.end()));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStreamW> OpenW(std::string_view) override {
        return nullptr;
    }

private:
    std::unordered_map<std::string, std::string> m_files;
};

wallpaper::WPSceneScriptRegistration MakeScriptRegistration(
    int32_t object_id,
    std::string property_name,
    wallpaper::WPDynamicValue::Type value_type,
    wallpaper::WPDynamicValue base_value,
    std::string script,
    wallpaper::SceneNode* node) {
    wallpaper::WPSceneScriptRegistration registration;
    registration.object_id = object_id;
    registration.object_name = "MigratedLayer";
    registration.property_name = std::move(property_name);
    registration.node = node;
    registration.target_kind = wallpaper::WPSceneScriptTargetKind::Layer;
    registration.value_type = value_type;
    registration.base_value = std::move(base_value);
    registration.setting.value = registration.base_value;
    registration.setting.script = std::move(script);
    return registration;
}

const wallpaper::vulkan::TextPass::Desc* FindTextPass(
    wallpaper::rg::RenderGraph& graph,
    int32_t layer_id,
    const std::string& output) {
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        if (auto* text = dynamic_cast<wallpaper::vulkan::TextPass*>(pass)) {
            if (text->desc().layer_id == layer_id && text->desc().output == output) {
                return &text->desc();
            }
        }
    }
    return nullptr;
}

const wallpaper::vulkan::ClearPass::Desc* FindClearPass(
    wallpaper::rg::RenderGraph& graph,
    const std::string& target) {
    for (auto id : graph.topologicalOrder()) {
        auto* pass = graph.getPass(id);
        if (auto* clear = dynamic_cast<wallpaper::vulkan::ClearPass*>(pass)) {
            if (clear->desc().target == target) {
                return &clear->desc();
            }
        }
    }
    return nullptr;
}

} // namespace

int main() {
    {
        wallpaper::fs::VFS vfs;
        wallpaper::WPShaderInfo shader_info;
        shader_info.combos["SHADER_PLACEHOLD"] = "1";
        std::array<wallpaper::WPShaderUnit, 2> units {
            wallpaper::WPShaderUnit {
                .stage = wallpaper::ShaderType::VERTEX,
                .src = R"(
                    attribute vec3 a_Position;
                    varying vec2 v_TexCoord;
                    void main() {
                        v_TexCoord = a_Position.xy;
                        gl_Position = vec4(a_Position, 1.0);
                    }
                )",
                .preprocess_info = {},
            },
            wallpaper::WPShaderUnit {
                .stage = wallpaper::ShaderType::FRAGMENT,
                .src = R"(
                    varying vec2 v_TexCoord;
                    uniform float u_BarCount;
                    void main() {
                        float frequency = floor(v_TexCoord.x * u_BarCount) / u_BarCount * 64.0;
                        uint barFreq1 = frequency % 64;
                        uint barFreq2 = (barFreq1 + 1) % 64;
                        float bar = step(v_TexCoord.y, float(barFreq2));
                        bool isLeftChannel = v_TexCoord.y < 0.49;
                        int masked = step(v_TexCoord.x, 0.5);
                        bar *= isLeftChannel;
                        gl_FragColor = vec4(bar + float(masked), float(barFreq1), 0.0, 1.0);
                    }
                )",
                .preprocess_info = {},
            },
        };
        std::vector<wallpaper::ShaderCode> codes;
        wallpaper::WPShaderParser::InitGlslang();
        const bool compiled = wallpaper::WPShaderParser::CompileToSpv(
            "migrated-shader-compat",
            std::span<wallpaper::WPShaderUnit>(units.data(), units.size()),
            codes,
            vfs,
            &shader_info,
            std::span<const wallpaper::WPShaderTexInfo>());
        wallpaper::WPShaderParser::FinalGlslang();
        assert(compiled);
        assert(codes.size() == units.size());
    }

    {
        wallpaper::wpscene::WPMaterial material;
        const bool parsed = material.FromJson(nlohmann::json {
            { "passes",
              nlohmann::json::array({
                  {
                      { "shader", "generic" },
                      { "textures", nlohmann::json::array({ nullptr }) },
                      { "usertextures",
                        nlohmann::json::array({
                            { { "name", "album_art" }, { "type", "project" } },
                        }) },
                      { "usershadervalues", { { "accent", "color1" } } },
                  },
              }) },
        });
        assert(parsed);
        assert(material.textures.size() == 1);
        assert(material.usertextures.size() == 1);
        assert(material.usertextures.front().name == "album_art");
        assert(material.usertextures.front().type == "project");
        assert(material.usershadervalues.at("accent") == "color1");

        wallpaper::SceneMaterial scene_material;
        scene_material.uniformAliases["color1"] = "g_Color1";
        wallpaper::SceneMaterial moved_material(std::move(scene_material));
        assert(moved_material.uniformAliases.at("color1") == "g_Color1");
    }

    {
        wallpaper::WPSceneParser parser;
        wallpaper::fs::VFS vfs;
        wallpaper::audio::SoundManager sound_manager;
        auto scene = parser.Parse("migrated-render-targets",
                                  R"({
                                      "camera": {
                                          "center": [0, 0, 0],
                                          "eye": [0, 0, 1],
                                          "up": [0, 1, 0]
                                      },
                                      "general": {
                                          "clearcolor": [0, 0, 0],
                                          "orthogonalprojection": {
                                              "width": 320,
                                              "height": 180
                                          },
                                          "zoom": 1
                                      },
                                      "objects": []
                                  })",
                                  vfs,
                                  sound_manager);
        assert(scene != nullptr);
        assert(scene->renderTargets.count(wallpaper::SpecTex_Default.data()) == 1);
        assert(scene->renderTargets.count("_rt_shadowAtlas") == 1);
        const auto& full_frame = scene->renderTargets.at(wallpaper::SpecTex_Default.data());
        assert(full_frame.width == 320);
        assert(full_frame.height == 180);
        assert(full_frame.mapWidth == 320);
        assert(full_frame.mapHeight == 180);
        const auto& shadow_atlas = scene->renderTargets.at("_rt_shadowAtlas");
        assert(shadow_atlas.withDepth);
        assert(shadow_atlas.allowReuse);
        assert(shadow_atlas.height == 180);
    }

    {
        wallpaper::WPSceneParser parser;
        wallpaper::fs::VFS vfs;
        assert(vfs.Mount(
            "/assets",
            std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
                { "/image.json",
                  R"({
                      "width": 16,
                      "height": 16,
                      "material": "materials/user_uniform.json"
                  })" },
                { "/materials/user_uniform.json",
                  R"({
                      "passes": [
                          {
                              "shader": "user_uniform",
                              "textures": [],
                              "usershadervalues": {
                                  "accent_color": "accent"
                              }
                          }
                      ]
                  })" },
                { "/shaders/user_uniform.vert",
                  R"(
                      attribute vec3 a_Position;
                      void main() {
                          gl_Position = vec4(a_Position, 1.0);
                      }
                  )" },
                { "/shaders/user_uniform.frag",
                  R"(
                      uniform vec3 g_AccentColor; // {"material":"accent","default":"0 0 0"}
                      void main() {
                          gl_FragColor = vec4(g_AccentColor, 1.0);
                      }
                  )" },
            }),
            "test-assets"));

        wallpaper::UserPropertyMap user_properties;
        user_properties.emplace(
            "accent_color",
            wallpaper::UserProperty {
                .value = wallpaper::ShaderValue(std::array<float, 3> { 0.9f, 0.4f, 0.2f }),
                .condition = {},
                .is_boolean = false,
            });

        wallpaper::audio::SoundManager sound_manager;
        auto scene = parser.Parse("migrated-user-materials",
                                  R"({
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
                                              "id": 42,
                                              "name": "UserMaterialLayer",
                                              "image": "image.json",
                                              "origin": [8, 8, 0],
                                              "angles": [0, 0, 0],
                                              "scale": [1, 1, 1]
                                          }
                                      ]
                                  })",
                                  vfs,
                                  sound_manager,
                                  &user_properties);
        assert(scene != nullptr);
        assert(scene->userProperties.count("accent_color") == 1);
        assert(scene->bindingRegistrations.size() == 1);
        const auto& registration = scene->bindingRegistrations.front();
        assert(registration.target_kind == wallpaper::WPSceneScriptTargetKind::MaterialUniform);
        assert(registration.object_id == 42);
        assert(registration.property_name == "g_AccentColor");
        assert(registration.node != nullptr);
        assert(registration.node->Mesh() != nullptr);
        auto* material = registration.node->Mesh()->Material();
        assert(material != nullptr);
        assert(material->customShader.constValues.count("g_AccentColor") == 1);
        const auto& accent = material->customShader.constValues.at("g_AccentColor");
        assert(accent.size() == 3);
        assert(NearlyEqual(accent[0], 0.9));
        assert(NearlyEqual(accent[1], 0.4));
        assert(NearlyEqual(accent[2], 0.2));
    }

    wallpaper::Scene scene;
    scene.vfs = std::make_unique<wallpaper::fs::VFS>();
    constexpr int32_t layer_id = 501;
    const std::string video_key = "media/movie.mp4";
    const std::string bridge_target = "_rt_text_migrated_bridge";

    scene.renderTargets[wallpaper::SpecTex_Default.data()] = { 128, 64, true };
    scene.renderTargets[bridge_target] = { 64, 32, true };

    wallpaper::SceneTexture video_texture;
    video_texture.isVideo = true;
    video_texture.width = 1920;
    video_texture.height = 1080;
    scene.textures.emplace(video_key, video_texture);

    auto node = std::make_shared<wallpaper::SceneNode>();
    node->ID() = layer_id;
    auto mesh = std::make_shared<wallpaper::SceneMesh>();
    wallpaper::SceneMaterial material;
    material.name = "migrated-material";
    material.textures.push_back(video_key);
    mesh->AddMaterial(std::move(material));
    node->AddMesh(mesh);
    scene.sceneGraph->AppendChild(node);

    wallpaper::RegisterSceneScriptBindingsForTest(
        scene,
        nlohmann::json {
            { "general",
              {
                  { "clearcolor",
                    { { "value", nlohmann::json::array({ 0.1f, 0.2f, 0.3f }) },
                      { "user", "scene_color" } } },
              } },
            { "objects",
              nlohmann::json::array({
                  {
                      { "id", layer_id },
                      { "name", "MigratedLayer" },
                      { "alpha", { { "value", 0.25f }, { "user", "layer_alpha" } } },
                      { "origin",
                        { { "value", nlohmann::json::array({ 1.0f, 2.0f, 3.0f }) },
                          { "script",
                            "export function update(value) { return value.add(new Vec3(1, 0, 0)); }" } } },
                  },
              }) },
        },
        node.get());
    assert(scene.bindingRegistrations.size() == 2);
    assert(scene.scriptRegistrations.size() == 1);
    assert(scene.scriptRegistrations.front().node == node.get());

    scene.scriptRegistrations.push_back(MakeScriptRegistration(
        layer_id,
        "brightness",
        wallpaper::WPDynamicValue::Type::Float,
        wallpaper::WPDynamicValue(0.5f),
        R"(
            export function update(value) {
                const video = thisLayer.getVideoTexture();
                if (video.isPlaying()) {
                    video.pause();
                }
                video.setCurrentTime(12.25);
                return value + 0.25;
            }
        )",
        node.get()));

    wallpaper::WPSceneScriptHost host(&scene);
    for (const auto& registration : scene.bindingRegistrations) {
        assert(host.RegisterPropertyBinding(registration));
    }
    for (const auto& registration : scene.scriptRegistrations) {
        assert(host.RegisterPropertyScript(registration));
    }
    host.Initialize();

    auto origin_value = host.FindResolvedValue(layer_id, "origin");
    assert(origin_value.has_value());
    std::array<float, 3> origin {};
    assert(origin_value->tryGet(&origin));
    assert(NearlyEqual(origin[0], 2.0));
    assert(NearlyEqual(origin[1], 2.0));
    assert(NearlyEqual(origin[2], 3.0));

    auto brightness_value = host.FindResolvedValue(layer_id, "brightness");
    assert(brightness_value.has_value());
    float brightness = 0.0f;
    assert(brightness_value->tryGet(&brightness));
    assert(NearlyEqual(brightness, 0.75));
    assert(scene.videoTexturePaused[video_key]);
    assert(scene.videoTextureStopped.count(video_key) == 0);
    assert(NearlyEqual(scene.videoTextureSeekRequests[video_key], 12.25));

    wallpaper::TextLayerRuntimeState text_state;
    text_state.object.id = layer_id;
    text_state.object.name = "MigratedText";
    text_state.object.text = "before";
    text_state.object.size = { 160.0f, 40.0f };
    text_state.object.size_explicit = true;
    std::string text_error;
    assert(wallpaper::BuildSceneTextPrimitive(
        *scene.vfs, text_state.object, 1, 1.0, &text_state.primitive, &text_error));
    assert(text_state.primitive != nullptr);
    assert(!text_state.primitive->layout.glyph_pages.empty());
    text_state.primitive->bridge.enabled = true;
    text_state.primitive->bridge.render_targets.push_back(
        wallpaper::TextBridgeRenderTarget { .name = bridge_target, .scale = 1 });

    assert(wallpaper::ApplyTextLayerPropertyValue(
        text_state,
        "text",
        wallpaper::WPDynamicValue(std::string("after"))));
    assert(text_state.object.text == "after");
    assert(text_state.primitive->object.text == "before");

    scene.textLayers[layer_id] = text_state;
    scene.textPrimitives[layer_id] = text_state.primitive;
    node->AddText(text_state.primitive);
    assert(wallpaper::RebuildTextLayerSceneLayout(scene, layer_id));
    assert(scene.dirtyTextLayerIds.count(layer_id) == 1);
    assert(scene.textLayers[layer_id].primitive->object.text == "after");
    assert(scene.textLayers[layer_id].primitive->atlas_version == 2);

    auto graph = wallpaper::BuildWESceneRenderPlan(scene);
    assert(FindClearPass(*graph, bridge_target) != nullptr);
    auto* text_pass = FindTextPass(*graph, layer_id, bridge_target);
    assert(text_pass != nullptr);
    assert(text_pass->node == node.get());
    assert(text_pass->clear_output);

    return 0;
}
