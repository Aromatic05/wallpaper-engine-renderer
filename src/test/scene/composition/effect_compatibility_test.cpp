#include "backend/scene/internal/SpecTexs.hpp"
#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/parser/WPShaderParser.hpp"
#include "backend/scene/internal/parser/effect/Extent.hpp"
#include "backend/scene/internal/parser/effect/LegacyAtmosphere.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneCamera.h"
#include "backend/scene/internal/scene/include/scene/SceneImageEffectLayer.h"
#include "backend/scene/internal/wpscene/WPEffect.h"
#include "backend/scene/internal/wpscene/WPImageObject.h"
#include "backend/scene/internal/wpscene/WPMaterial.h"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"
#include "timer/FrameTimer.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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
                 "effect compatibility test failure: %.*s\n",
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
    const std::string fragment_shader = R"(
        uniform sampler2D g_Texture0; // {"material":"previous"}
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = texture(g_Texture0, v_TexCoord);
        }
    )";

    Require(vfs.Mount(
                "/assets",
                std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
                    { "/zero.json",
                      R"({
                          "width": 0,
                          "height": 0,
                          "material": "materials/image.json"
                      })" },
                    { "/small.json",
                      R"({
                          "width": 8,
                          "height": 4,
                          "material": "materials/image.json"
                      })" },
                    { "/materials/image.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "genericimage",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/effects/filter.json",
                      R"({
                          "name": "Filter",
                          "passes": [
                              { "material": "materials/filter.json" }
                          ],
                          "fbos": [
                              {
                                  "name": "unique_buffer",
                                  "format": "rgba8",
                                  "scale": 2,
                                  "unique": true
                              }
                          ]
                      })" },
                    { "/effects/broken.json",
                      R"({
                          "name": "Broken",
                          "passes": [
                              { "material": "materials/broken.json" }
                          ]
                      })" },
                    { "/materials/filter.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "effects/filter",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/materials/broken.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "effects/broken",
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
                    { "/shaders/genericimage.frag", fragment_shader },
                    { "/shaders/effects/filter.vert", vertex_shader },
                    { "/shaders/effects/filter.frag", fragment_shader },
                    { "/shaders/effects/broken.vert", vertex_shader },
                    { "/shaders/effects/broken.frag", "this is not valid shader source" },
                    { "/shaders/passthrough.vert", vertex_shader },
                    { "/shaders/passthrough.frag", fragment_shader },
                }),
                "effect-compat-assets"),
            "failed to mount effect compatibility assets");
}

std::shared_ptr<wallpaper::Scene> ParseScene() {
    wallpaper::WPSceneParser parser;
    wallpaper::fs::VFS vfs;
    wallpaper::audio::SoundManager sound_manager;
    MountAssets(vfs);

    return parser.Parse("effect-compatibility",
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
                            "objects": [
                                {
                                    "id": 10,
                                    "name": "ZeroExtent",
                                    "image": "zero.json",
                                    "origin": [0, 0, 0],
                                    "angles": [0, 0, 0],
                                    "scale": [1, 1, 1],
                                    "effects": [
                                        {
                                            "id": 11,
                                            "file": "effects/filter.json",
                                            "visible": true
                                        }
                                    ]
                                },
                                {
                                    "id": 20,
                                    "name": "PassthroughExtent",
                                    "image": "small.json",
                                    "origin": [0, 0, 0],
                                    "angles": [0, 0, 0],
                                    "scale": [1, 1, 1],
                                    "config": { "passthrough": true },
                                    "effects": [
                                        {
                                            "id": 21,
                                            "file": "effects/filter.json",
                                            "visible": true
                                        }
                                    ]
                                },
                                {
                                    "id": 30,
                                    "name": "BrokenEffectChain",
                                    "image": "small.json",
                                    "origin": [0, 0, 0],
                                    "angles": [0, 0, 0],
                                    "scale": [1, 1, 1],
                                    "effects": [
                                        {
                                            "id": 31,
                                            "file": "effects/broken.json",
                                            "visible": true
                                        },
                                        {
                                            "id": 32,
                                            "file": "effects/filter.json",
                                            "visible": true
                                        }
                                    ]
                                }
                            ]
                        })",
                        vfs,
                        sound_manager);
}

const wallpaper::SceneRenderTarget* FirstLayerTarget(const wallpaper::Scene& scene,
                                                       int32_t layer_id) {
    const auto it = scene.objectRuntimeRenderTargets.find(layer_id);
    if (it == scene.objectRuntimeRenderTargets.end() || it->second.empty()) return nullptr;
    const auto target_it = scene.renderTargets.find(it->second.front());
    return target_it == scene.renderTargets.end() ? nullptr : &target_it->second;
}

wallpaper::SceneImageEffectLayer* FindEffectLayer(wallpaper::Scene& scene, int32_t layer_id) {
    const auto nodes_it = scene.objectRuntimeNodes.find(layer_id);
    if (nodes_it == scene.objectRuntimeNodes.end()) return nullptr;
    for (auto* node : nodes_it->second) {
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
} // namespace

int main() {
    Require(wallpaper::NonZeroRenderTargetDimension(0.0f) == 1,
            "zero render target dimension was not clamped");
    Require(wallpaper::NonZeroRenderTargetDimension(-8.0f) == 1,
            "negative render target dimension was not clamped");
    Require(wallpaper::NonZeroRenderTargetDimension(
                std::numeric_limits<float>::quiet_NaN()) == 1,
            "NaN render target dimension was not clamped");
    Require(wallpaper::NonZeroRenderTargetExtent(16.4f, 8.6f) ==
                std::array<int32_t, 2> { 16, 8 },
            "positive render target extent was not truncated consistently");

    wallpaper::wpscene::WPEffectFbo fbo;
    Require(fbo.FromJson(nlohmann::json {
                { "name", "unique" },
                { "format", "rgba8" },
                { "scale", 2 },
                { "unique", true },
            }),
            "effect FBO JSON failed to parse");
    Require(fbo.unique, "effect FBO unique flag was not preserved");
    Require(fbo.ResolveSize({ 0.0f, -1.0f }) == std::array<int32_t, 2> { 1, 1 },
            "effect FBO size did not clamp invalid source extents");

    wallpaper::wpscene::WPMaterial atmosphere;
    atmosphere.shader = "workshop/2839476907/effects/atmosphere";
    atmosphere.combos["LIGHT1"] = 1;
    atmosphere.constantshadervalues["Planet position"] = { 1.0f, 2.0f, 3.0f };
    atmosphere.constantshadervalues["Planet radius"] = { 4.0f };

    wallpaper::WPShaderInfo atmosphere_info;
    atmosphere_info.alias["Position"] = "g_Position";
    atmosphere_info.alias["Planet size"] = "g_PlanetRadius";
    atmosphere_info.combos["LIGHT_INDEX"] = "0";
    wallpaper::ApplyLegacyAtmosphereUniformAliases(atmosphere, atmosphere_info);
    wallpaper::ApplyLegacyAtmosphereLightCombo(atmosphere, atmosphere_info);
    Require(atmosphere_info.alias.count("Position") == 0 &&
                atmosphere_info.alias.at("Planet position") == "g_Position",
            "legacy atmosphere position alias was not preferred");
    Require(atmosphere_info.alias.count("Planet size") == 0 &&
                atmosphere_info.alias.at("Planet radius") == "g_PlanetRadius",
            "legacy atmosphere radius alias was not preferred");
    Require(atmosphere_info.combos.at("LIGHT_INDEX") == "4",
            "legacy atmosphere light index was not selected");
    Require(atmosphere_info.baseConstSvs.count(std::string(wallpaper::G_VIEWFORWARD)) == 1,
            "legacy atmosphere view-forward uniform was not seeded");
    Require(wallpaper::IsLegacyAtmosphereShadowValue(atmosphere, "Radius"),
            "legacy atmosphere shadow value was not recognized");

    std::array units {
        wallpaper::WPShaderUnit {
            .stage = wallpaper::ShaderType::VERTEX,
            .src = "void main() {}",
            .preprocess_info = {},
        },
        wallpaper::WPShaderUnit {
            .stage = wallpaper::ShaderType::FRAGMENT,
            .src = "float pointDensity, opticalDepth;\n"
                   "float localDensity, cameraOpticalDepth, sunRayLength, sunOpticalDepth, "
                   "lightInstensity = 1.0;",
            .preprocess_info = {},
        },
    };
    wallpaper::ApplyLegacyAtmosphereShaderCompat(atmosphere, units);
    Require(units[1].src.find("pointDensity = 0.0") != std::string::npos &&
                units[1].src.find("cameraOpticalDepth = 0.0") != std::string::npos,
            "legacy atmosphere shader locals were not initialized");

    wallpaper::FrameTimer timer;
    timer.SetRequiredFps(60);
    Require(timer.RequiredFps() == 60, "frame timer did not keep requested FPS");
    Require(std::abs(timer.IdeaTime() - 1.0 / 60.0) < 2e-6,
            "frame timer lost microsecond precision");
    timer.SetRequiredFps(0);
    Require(timer.RequiredFps() == 1 && std::abs(timer.IdeaTime() - 1.0) < 2e-6,
            "frame timer did not clamp zero FPS");

    auto scene = ParseScene();
    Require(scene != nullptr, "effect compatibility scene failed to parse");
    const auto* zero_target = FirstLayerTarget(*scene, 10);
    Require(zero_target != nullptr && zero_target->width == 1 && zero_target->height == 1,
            "zero-sized image effect target was not clamped to 1x1");
    const auto* passthrough_target = FirstLayerTarget(*scene, 20);
    Require(passthrough_target != nullptr && passthrough_target->width == 320 &&
                passthrough_target->height == 180,
            "passthrough effect target did not use active camera extent");

    bool found_unique_target = false;
    for (const auto& [name, target] : scene->renderTargets) {
        if (name.find("unique_buffer_") == std::string::npos) continue;
        found_unique_target = true;
        Require(!target.allowReuse, "unique effect FBO remained reusable");
    }
    Require(found_unique_target, "unique effect FBO was not materialized");

    auto* broken_effect_layer = FindEffectLayer(*scene, 30);
    Require(broken_effect_layer != nullptr, "broken effect chain has no effect bridge");
    Require(broken_effect_layer->EffectCount() == 0,
            "downstream effects remained active after an earlier effect failed to load");

    return 0;
}
