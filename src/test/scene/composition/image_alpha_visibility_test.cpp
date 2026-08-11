#include "backend/scene/internal/engine/WESceneRenderPlanBuilder.hpp"
#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/scenescript/WPSceneScriptHost.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneCamera.h"
#include "backend/scene/internal/scene/include/scene/SceneImageEffectLayer.h"
#include "backend/scene/internal/scene/include/scene/SceneMaterial.h"
#include "backend/scene/internal/scene/include/scene/SceneMesh.h"
#include "backend/scene/internal/settings/WPUserProperties.hpp"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"
#include "rendergraph/RenderGraph.hpp"

#include <cmath>
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

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "image alpha visibility test failure: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

bool NearlyEqual(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.0001f;
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
        uniform vec4 g_Color4;
        uniform float g_UserAlpha;
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = vec4(g_Color4.rgb, g_UserAlpha);
        }
    )";
    const std::string transform_fragment_shader = R"(
        uniform sampler2D g_Texture0;
        uniform float g_UserAlpha;
        uniform float g_Speed; // {"material":"speed","default":0.5}
        varying vec2 v_TexCoord;
        void main() {
            vec4 sampled = texture(g_Texture0, v_TexCoord);
            sampled.rgb *= 1.0 + g_Speed * 0.001;
            gl_FragColor = vec4(sampled.rgb, sampled.a * g_UserAlpha);
        }
    )";
    const std::string passthrough_fragment_shader = R"(
        uniform sampler2D g_Texture0;
        uniform float g_UserAlpha;
        varying vec2 v_TexCoord;
        void main() {
            vec4 sampled = texture(g_Texture0, v_TexCoord);
            gl_FragColor = vec4(sampled.rgb, sampled.a * g_UserAlpha);
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
                    { "/effects/transform.json",
                      R"({
                          "name": "Transform",
                          "passes": [
                              { "material": "materials/transform.json" }
                          ]
                      })" },
                    { "/materials/transform.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "effects/transform",
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
                    { "/shaders/effects/transform.vert", vertex_shader },
                    { "/shaders/effects/transform.frag", transform_fragment_shader },
                    { "/shaders/passthrough.vert", vertex_shader },
                    { "/shaders/passthrough.frag", passthrough_fragment_shader },
                }),
                "image-alpha-assets"),
            "failed to mount image alpha assets");
}

wallpaper::UserProperty NumericProperty(float value) {
    return wallpaper::UserProperty {
        .value = wallpaper::ShaderValue(value),
        .condition = {},
        .is_boolean = false,
    };
}

wallpaper::UserProperty BooleanProperty(bool value) {
    return wallpaper::UserProperty {
        .value = wallpaper::ShaderValue(value ? 1.0f : 0.0f),
        .condition = {},
        .is_boolean = true,
    };
}

wallpaper::UserPropertyMap Properties(bool show_direct, float direct_alpha, float effect_alpha) {
    wallpaper::UserPropertyMap properties;
    properties.emplace("show_direct", BooleanProperty(show_direct));
    properties.emplace("direct_alpha", NumericProperty(direct_alpha));
    properties.emplace("effect_alpha", NumericProperty(effect_alpha));
    return properties;
}

struct Fixture {
    // Scene keeps a raw SoundManager pointer and takes ownership of the VFS only after parsing,
    // matching WESceneRuntimeDriver's lifetime contract. Keep the host alive longer than Scene.
    std::unique_ptr<wallpaper::audio::SoundManager> sound_manager;
    std::shared_ptr<wallpaper::Scene> scene;
};

Fixture ParseScene(const wallpaper::UserPropertyMap& properties) {
    wallpaper::WPSceneParser parser;
    auto vfs = std::make_unique<wallpaper::fs::VFS>();
    auto sound_manager = std::make_unique<wallpaper::audio::SoundManager>();
    MountAssets(*vfs);

    auto scene = parser.Parse("image-alpha-visibility",
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
                                          "name": "DeferredAlpha",
                                          "image": "image.json",
                                          "origin": [32, 32, 0],
                                          "angles": [0, 0, 0],
                                          "scale": {
                                              "value": [1, 1, 1],
                                              "script": "'use strict';\nexport function mediaPlaybackChanged(event) { shared.mCheckRotation(event); }\nexport function update(value) { return new Vec3(shared.lastEffectInitCount ?? -1, 1, 1); }"
                                          },
                                          "visible": { "value": false, "user": "show_direct" },
                                          "alpha": { "value": 0.25, "user": "direct_alpha" },
                                          "effects": [
                                              {
                                                  "id": 11,
                                                  "file": "effects/transform.json",
                                                  "visible": true,
                                                  "passes": [
                                                      {
                                                          "constantshadervalues": {
                                                              "speed": {
                                                                  "value": 0.5,
                                                                  "script": "'use strict';\nlet speed = 0.5;\nshared.effectModuleInitCount = (shared.effectModuleInitCount ?? 0) + 1;\nshared.mCheckRotation = (event) => { shared.lastEffectInitCount = shared.effectModuleInitCount; speed = 0.75; };\nexport function update(value) { return speed; }"
                                                              }
                                                          }
                                                      }
                                                  ]
                                              }
                                          ]
                                      },
                                      {
                                          "id": 20,
                                          "name": "EffectAlpha",
                                          "image": "image.json",
                                          "origin": [96, 32, 0],
                                          "angles": [0, 0, 0],
                                          "scale": [1, 1, 1],
                                          "visible": true,
                                          "alpha": { "value": 0.4, "user": "effect_alpha" },
                                          "effects": [
                                              {
                                                  "id": 21,
                                                  "file": "effects/transform.json",
                                                  "visible": true
                                              }
                                          ]
                                      }
                                  ]
                              })",
                              *vfs,
                              *sound_manager,
                              &properties);
    if (scene != nullptr) scene->vfs = std::move(vfs);
    return Fixture { .sound_manager = std::move(sound_manager), .scene = std::move(scene) };
}

float ReadAlpha(const wallpaper::SceneMaterial& material) {
    for (const std::string name : { "g_UserAlpha", "g_Alpha" }) {
        const auto it = material.customShader.constValues.find(name);
        if (it != material.customShader.constValues.end() && it->second.size() == 1) {
            return it->second[0];
        }
    }
    const auto color = material.customShader.constValues.find("g_Color4");
    if (color != material.customShader.constValues.end() && color->second.size() == 4) {
        return color->second[3];
    }
    Fail("material has no alpha uniform");
}

wallpaper::SceneMaterial* FindBaseMaterial(wallpaper::Scene& scene, int32_t layer_id) {
    const auto nodes_it = scene.objectRuntimeNodes.find(layer_id);
    if (nodes_it == scene.objectRuntimeNodes.end()) return nullptr;
    for (auto* node : nodes_it->second) {
        if (node != nullptr && node->Mesh() != nullptr && node->Mesh()->Material() != nullptr) {
            return node->Mesh()->Material();
        }
    }
    return nullptr;
}

wallpaper::SceneMaterial* FindAuthoredFinalEffectMaterial(wallpaper::Scene& scene,
                                                          int32_t layer_id) {
    const auto cameras_it = scene.objectRuntimeCameraNames.find(layer_id);
    if (cameras_it == scene.objectRuntimeCameraNames.end()) return nullptr;
    for (const auto& camera_name : cameras_it->second) {
        const auto camera_it = scene.cameras.find(camera_name);
        if (camera_it == scene.cameras.end() || camera_it->second == nullptr ||
            !camera_it->second->HasImgEffect()) {
            continue;
        }
        auto* layer = camera_it->second->GetImgEffect().get();
        if (layer == nullptr || layer->EffectCount() == 0) continue;
        auto& effect = layer->GetEffect(layer->EffectCount() - 1);
        if (effect->nodes.empty()) continue;
        auto* node = effect->nodes.back().sceneNode.get();
        if (node != nullptr && node->Mesh() != nullptr) return node->Mesh()->Material();
    }
    return nullptr;
}

void RegisterRuntime(wallpaper::Scene& scene) {
    scene.scriptHost = std::make_shared<wallpaper::WPSceneScriptHost>(&scene);
    Require(scene.scriptHost->Ready(), "SceneScript host failed to initialize");
    for (const auto& registration : scene.bindingRegistrations) {
        Require(scene.scriptHost->RegisterPropertyBinding(registration),
                "failed to register property binding");
    }
    for (const auto& registration : scene.propertyAnimationRegistrations) {
        Require(scene.scriptHost->RegisterPropertyAnimation(registration),
                "failed to register property animation");
    }
    for (const auto& registration : scene.scriptRegistrations) {
        Require(scene.scriptHost->RegisterPropertyScript(registration),
                "failed to register property script");
    }
    scene.scriptHost->Initialize();
}

} // namespace

int main() {
    auto initial = Properties(false, 0.35f, 0.45f);
    auto fixture = ParseScene(initial);
    auto scene = fixture.scene;
    Require(scene != nullptr, "scene failed to parse");
    Require(scene->deferredRuntimeImageLayerIds.contains(10),
            "hidden user-visible image was not deferred");
    Require(!scene->GetLayerLocalVisibility(10),
            "hidden user-visible image started visible");

    auto* effect_base = FindBaseMaterial(*scene, 20);
    auto* effect_final = FindAuthoredFinalEffectMaterial(*scene, 20);
    Require(effect_base != nullptr && effect_final != nullptr,
            "effect-backed image materials were not created");
    Require(NearlyEqual(ReadAlpha(*effect_base), 0.45f),
            "initial user alpha did not reach effect source material");
    Require(NearlyEqual(ReadAlpha(*effect_final), 0.45f),
            "initial user alpha did not reach authored final effect material");

    auto graph = wallpaper::BuildWESceneRenderPlan(*scene);
    Require(graph != nullptr, "initial render graph failed to build");
    RegisterRuntime(*scene);
    scene->scriptHost->FrameBegin(0.1);
    Require(NearlyEqual(scene->layerNodes.at(10)->Scale().x(), 1.0f),
            "deferred effect script module was not initialized before initial media dispatch");

    auto shown = Properties(true, 0.7f, 0.6f);
    scene->scriptHost->ApplyUserProperties(shown, false);
    Require(scene->GetLayerLocalVisibility(10),
            "visible user property did not show deferred image");
    Require(!scene->deferredRuntimeImageLayerIds.contains(10),
            "shown image remained deferred");

    auto* direct_material = FindBaseMaterial(*scene, 10);
    effect_base = FindBaseMaterial(*scene, 20);
    effect_final = FindAuthoredFinalEffectMaterial(*scene, 20);
    Require(direct_material != nullptr, "shown image has no material");
    Require(NearlyEqual(ReadAlpha(*direct_material), 0.7f),
            "same-dispatch alpha did not reach newly materialized image");
    auto* deferred_effect = FindAuthoredFinalEffectMaterial(*scene, 10);
    Require(deferred_effect != nullptr, "shown deferred image has no effect material");
    const auto speed = deferred_effect->customShader.constValues.find("g_Speed");
    Require(speed != deferred_effect->customShader.constValues.end() && speed->second.size() == 1 &&
                NearlyEqual(speed->second[0], 0.75f),
            "deferred effect script state was not applied to the materialized uniform");
    Require(NearlyEqual(ReadAlpha(*effect_base), 0.6f),
            "dynamic alpha did not reach effect source material");
    Require(NearlyEqual(ReadAlpha(*effect_final), 0.6f),
            "dynamic alpha did not reach authored final effect material");

    wallpaper::WPSceneScriptMediaState media_state;
    media_state.playback_state = 1;
    scene->scriptHost->ApplyMediaState(media_state, false);
    scene->scriptHost->FrameBegin(0.1);
    Require(NearlyEqual(scene->layerNodes.at(10)->Scale().x(), 1.0f),
            "deferred effect script module was initialized more than once after materialization");

    auto hidden = Properties(false, 0.2f, 0.6f);
    scene->scriptHost->ApplyUserProperties(hidden, false);
    Require(!scene->GetLayerLocalVisibility(10), "visible user property did not hide image");
    Require(NearlyEqual(ReadAlpha(*direct_material), 0.2f),
            "hidden image did not retain live alpha updates");

    auto reshown = Properties(true, 0.2f, 0.6f);
    scene->scriptHost->ApplyUserProperties(reshown, false);
    Require(scene->GetLayerLocalVisibility(10), "image did not become visible again");
    Require(NearlyEqual(ReadAlpha(*direct_material), 0.2f),
            "visibility restore reset the authored alpha");

    return 0;
}
