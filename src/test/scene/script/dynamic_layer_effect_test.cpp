#include "backend/scene/internal/engine/WESceneRenderPlanBuilder.hpp"
#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/scenescript/WPSceneScriptHost.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneCamera.h"
#include "backend/scene/internal/scene/include/scene/SceneImageEffectLayer.h"
#include "backend/scene/internal/scene/include/scene/SceneMaterial.h"
#include "backend/scene/internal/scene/include/scene/SceneMesh.h"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"
#include "render/vulkanrender/CustomShaderPass.hpp"
#include "rendergraph/RenderGraph.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
class MemoryFs final : public wallpaper::fs::Fs {
public:
    explicit MemoryFs(std::unordered_map<std::string, std::string> files)
        : m_files(std::move(files)) {}

    bool Contains(std::string_view path) const override {
        return m_files.contains(std::string(path));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStream> Open(std::string_view path) override {
        const auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        return std::make_shared<wallpaper::fs::MemBinaryStream>(
            std::vector<std::uint8_t>(it->second.begin(), it->second.end()));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStreamW> OpenW(std::string_view) override {
        return nullptr;
    }

private:
    std::unordered_map<std::string, std::string> m_files;
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
    const std::string blur_fragment_shader = R"(
        uniform sampler2D g_Texture0; // {"material":"previous"}
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = texture(g_Texture0, v_TexCoord);
        }
    )";
    const std::string passthrough_fragment_shader = R"(
        uniform sampler2D g_Texture0;
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = texture(g_Texture0, v_TexCoord);
        }
    )";

    assert(vfs.Mount(
        "/assets",
        std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
            { "/image.json",
              R"({"width":64,"height":32,"material":"materials/image.json"})" },
            { "/materials/image.json",
              R"({"passes":[{"shader":"genericimage","textures":[],"blending":"translucent"}]})" },
            { "/effects/blur.json",
              R"({"name":"Dynamic Blur","passes":[{"material":"materials/blur.json"}]})" },
            { "/materials/blur.json",
              R"({"passes":[{"shader":"effects/blur","textures":[]}]})" },
            { "/materials/util/effectpassthrough.json",
              R"({"passes":[{"shader":"passthrough","textures":[]}]})" },
            { "/shaders/genericimage.vert", vertex_shader },
            { "/shaders/genericimage.frag", image_fragment_shader },
            { "/shaders/effects/blur.vert", vertex_shader },
            { "/shaders/effects/blur.frag", blur_fragment_shader },
            { "/shaders/passthrough.vert", vertex_shader },
            { "/shaders/passthrough.frag", passthrough_fragment_shader },
        }),
        "dynamic-layer-effect-assets"));
}

wallpaper::WPSceneScriptRegistration MakeRegistration(int32_t object_id,
                                                       wallpaper::SceneNode* node) {
    wallpaper::WPSceneScriptRegistration registration;
    registration.object_id = object_id;
    registration.object_name = "Root";
    registration.property_name = "alpha";
    registration.target_kind = wallpaper::WPSceneScriptTargetKind::Layer;
    registration.value_type = wallpaper::WPDynamicValue::Type::Float;
    registration.base_value = wallpaper::WPDynamicValue(1.0f);
    registration.setting.value = registration.base_value;
    registration.node = node;
    return registration;
}

wallpaper::SceneImageEffectLayer* FindEffectLayer(wallpaper::Scene& scene, int32_t layer_id) {
    const auto nodes_it = scene.objectRuntimeNodes.find(layer_id);
    if (nodes_it == scene.objectRuntimeNodes.end()) return nullptr;
    for (auto* node : nodes_it->second) {
        if (node == nullptr || node->Camera().empty()) continue;
        const auto camera_it = scene.cameras.find(node->Camera());
        if (camera_it != scene.cameras.end() && camera_it->second != nullptr
            && camera_it->second->HasImgEffect()) {
            return camera_it->second->GetImgEffect().get();
        }
    }
    return nullptr;
}

bool HasEffectMaterial(wallpaper::rg::RenderGraph& graph, std::string_view material_name) {
    for (const auto id : graph.topologicalOrder()) {
        const auto* pass = dynamic_cast<const wallpaper::vulkan::CustomShaderPass*>(graph.getPass(id));
        if (pass == nullptr || pass->desc().node == nullptr
            || pass->desc().node->Mesh() == nullptr
            || pass->desc().node->Mesh()->Material() == nullptr) {
            continue;
        }
        if (pass->desc().node->Mesh()->Material()->name == material_name) return true;
    }
    return false;
}

struct DynamicIds {
    int32_t parent { 0 };
    int32_t child { 0 };
};

DynamicIds ResolveIds(const wallpaper::Scene& scene) {
    DynamicIds ids;
    if (const auto it = scene.layerNameToId.find("DynamicAttachment");
        it != scene.layerNameToId.end()) {
        ids.parent = it->second;
    }
    if (const auto it = scene.layerNameToId.find("DynamicEffectImage");
        it != scene.layerNameToId.end()) {
        ids.child = it->second;
    }
    return ids;
}

void RequireLiveDynamicTree(wallpaper::Scene& scene, const DynamicIds& ids) {
    assert(ids.parent > 0 && ids.child > 0 && ids.parent != ids.child);
    assert(scene.layerNodes.contains(ids.parent) && scene.layerNodes.at(ids.parent) != nullptr);
    assert(scene.layerNodes.contains(ids.child) && scene.layerNodes.at(ids.child) != nullptr);
    assert(scene.GetLayerParentBinding(ids.child).parent_id == ids.parent);
    assert(scene.imageLayers.contains(ids.child));
    assert(scene.initialLayerConfigJson.contains(ids.parent));
    assert(scene.initialLayerConfigJson.contains(ids.child));

    auto* effect_layer = FindEffectLayer(scene, ids.child);
    assert(effect_layer != nullptr);
    assert(effect_layer->EffectCount() == 1);
    assert(effect_layer->GetEffect(0)->EffectName() == "Dynamic Blur");

    auto graph = wallpaper::BuildWESceneRenderPlan(scene);
    assert(graph != nullptr);
    assert(HasEffectMaterial(*graph, "effects/blur"));
}
} // namespace

int main() {
    auto vfs = std::make_unique<wallpaper::fs::VFS>();
    MountAssets(*vfs);
    auto sound_manager = std::make_unique<wallpaper::audio::SoundManager>();

    wallpaper::WPSceneParser parser;
    auto scene = parser.Parse(
        "dynamic-effect-layer",
        R"({
            "camera":{"center":[0,0,0],"eye":[0,0,1],"up":[0,1,0]},
            "general":{"clearcolor":[0,0,0],"orthogonalprojection":{"width":256,"height":128},"zoom":1},
            "objects":[{
                "id":1,"name":"Root","origin":[0,0,0],"angles":[0,0,0],
                "scale":[1,1,1],"visible":true
            }]
        })",
        *vfs,
        *sound_manager);
    assert(scene != nullptr);
    scene->vfs = std::move(vfs);

    auto* root_node = scene->layerNodes.at(1);
    assert(root_node != nullptr);
    auto root_mesh = std::make_shared<wallpaper::SceneMesh>();
    wallpaper::SceneMaterial root_material;
    root_material.customShader.constValues["g_UserAlpha"] = wallpaper::ShaderValue(1.0f);
    root_mesh->AddMaterial(std::move(root_material));
    root_node->AddMesh(root_mesh);

    wallpaper::WPSceneScriptHost host(scene.get());
    auto registration = MakeRegistration(1, root_node);
    registration.setting.script = R"(
        let stage = 0;
        export function update(value) {
            if (stage === 0 || stage === 3) {
                const attachment = thisScene.createLayer({
                    name: 'DynamicAttachment', origin: [10, 20, 0], angles: [0, 0, 0],
                    scale: [1, 1, 1], visible: true
                });
                const image = thisScene.createLayer({
                    name: 'DynamicEffectImage', image: 'image.json', parent: 'DynamicAttachment',
                    origin: [4, 5, 0], angles: [0, 0, 0], scale: [1, 1, 1], visible: true,
                    effects: [{ id: 9001, file: 'effects/blur.json', visible: true }]
                });
                if (!attachment || !image) return 0;
                stage += 1;
            } else if (stage === 1) {
                thisScene.destroyLayer(thisScene.getLayer('DynamicEffectImage'));
                thisScene.destroyLayer(thisScene.getLayer('DynamicAttachment'));
                stage = 2;
            } else if (stage === 2) {
                stage = 3;
            }
            return value;
        }
    )";
    assert(host.RegisterPropertyScript(std::move(registration)));
    host.Initialize();

    host.FrameBegin(0.1);
    const DynamicIds first = ResolveIds(*scene);
    RequireLiveDynamicTree(*scene, first);

    host.FrameBegin(0.1);
    assert(ResolveIds(*scene).child == first.child);

    host.FrameBegin(0.1);
    assert(ResolveIds(*scene).parent == 0);
    assert(ResolveIds(*scene).child == 0);
    assert(!scene->layerNodes.contains(first.parent));
    assert(!scene->layerNodes.contains(first.child));
    assert(!scene->imageLayers.contains(first.child));
    assert(!scene->objectRuntimeNodes.contains(first.parent));
    assert(!scene->objectRuntimeNodes.contains(first.child));
    assert(FindEffectLayer(*scene, first.child) == nullptr);
    auto destroyed_graph = wallpaper::BuildWESceneRenderPlan(*scene);
    assert(destroyed_graph != nullptr);
    assert(!HasEffectMaterial(*destroyed_graph, "effects/blur"));

    host.FrameBegin(0.1);
    const DynamicIds second = ResolveIds(*scene);
    RequireLiveDynamicTree(*scene, second);
    return 0;
}
