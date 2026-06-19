#include "backend/scene/internal/scenescript/WPSceneScriptHost.hpp"

#include <cassert>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneMaterial.h"
#include "backend/scene/internal/scene/include/scene/SceneMesh.h"
#include "backend/scene/internal/scene/include/scene/SceneNode.h"
#include "backend/scene/internal/scene/include/scene/SceneTexture.h"
#include "backend/scene/internal/shader/WPShaderValueUpdater.hpp"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"

namespace
{

bool NearlyEqual(double lhs, double rhs, double epsilon = 0.0001) {
    return std::abs(lhs - rhs) <= epsilon;
}

wallpaper::WPSceneScriptRegistration MakeRegistration(
    int32_t object_id,
    std::string object_name,
    std::string property_name,
    wallpaper::WPSceneScriptTargetKind target_kind,
    wallpaper::WPDynamicValue::Type value_type,
    wallpaper::WPDynamicValue base_value) {
    wallpaper::WPSceneScriptRegistration registration;
    registration.object_id = object_id;
    registration.object_name = std::move(object_name);
    registration.property_name = std::move(property_name);
    registration.target_kind = target_kind;
    registration.value_type = value_type;
    registration.base_value = std::move(base_value);
    registration.setting.value = registration.base_value;
    return registration;
}

void BindToUserProperty(wallpaper::WPSceneScriptRegistration& registration, std::string name) {
    wallpaper::UserPropertyBinding binding;
    binding.name = std::move(name);
    registration.setting.property = std::move(binding);
}

std::shared_ptr<wallpaper::SceneNode> MakeLayerNode(int32_t id, std::string name) {
    auto node = std::make_shared<wallpaper::SceneNode>();
    node->ID() = id;
    node->SetName(std::move(name));
    return node;
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

void RegisterLayer(wallpaper::Scene& scene,
                   int32_t id,
                   const std::shared_ptr<wallpaper::SceneNode>& node,
                   std::string config_json = "{}") {
    scene.layerOrder.push_back(id);
    scene.layerNodes[id] = node.get();
    scene.layerNameToId[node->Name()] = id;
    scene.initialLayerConfigJson[id] = std::move(config_json);
    scene.objectRuntimeNodes[id].push_back(node.get());
    scene.nodeOwners[node.get()] = id;
}

} // namespace

int main() {
    using wallpaper::Scene;
    using wallpaper::SceneMaterial;
    using wallpaper::SceneMesh;
    using wallpaper::SceneTexture;
    using wallpaper::ShaderValue;
    using wallpaper::UserProperty;
    using wallpaper::UserPropertyMap;
    using wallpaper::WPDynamicValue;
    using wallpaper::WPSceneScriptHost;
    using wallpaper::WPSceneScriptTargetKind;

    {
        Scene scene;
        auto node = MakeLayerNode(9, "MaterialLayer");
        auto mesh = std::make_shared<SceneMesh>();
        SceneMaterial material;
        material.uniformAliases["accent"] = "g_AccentColor";
        material.customShader.constValues["g_AccentColor"] =
            ShaderValue(std::array<float, 3> { 0.1f, 0.2f, 0.3f });
        mesh->AddMaterial(std::move(material));
        node->AddMesh(mesh);

        WPSceneScriptHost host(&scene);
        assert(host.Ready());

        auto registration = MakeRegistration(9,
                                             "MaterialLayer",
                                             "g_AccentColor",
                                             WPSceneScriptTargetKind::MaterialUniform,
                                             WPDynamicValue::Type::Float3,
                                             WPDynamicValue(std::array<float, 3> {
                                                 0.1f,
                                                 0.2f,
                                                 0.3f,
                                             }));
        registration.node = node.get();
        BindToUserProperty(registration, "accent_color");
        assert(host.RegisterPropertyBinding(std::move(registration)));
        host.Initialize();

        UserPropertyMap properties;
        properties.emplace(
            "accent_color",
            UserProperty {
                .value = ShaderValue(std::array<float, 3> { 0.8f, 0.6f, 0.4f }),
                .condition = {},
                .is_boolean = false,
            });
        host.ApplyUserProperties(properties, false);

        const auto* updated_material = node->Mesh()->Material();
        assert(updated_material != nullptr);
        assert(updated_material->customShader.constValues.count("g_AccentColor") == 1);
        const auto& accent = updated_material->customShader.constValues.at("g_AccentColor");
        assert(accent.size() == 3);
        assert(NearlyEqual(accent[0], 0.8));
        assert(NearlyEqual(accent[1], 0.6));
        assert(NearlyEqual(accent[2], 0.4));
    }

    {
        Scene scene;
        SceneTexture texture;
        texture.isVideo = true;
        scene.textures.emplace("movie", texture);

        auto node = MakeLayerNode(20, "VideoLayer");
        auto mesh = std::make_shared<SceneMesh>();
        SceneMaterial material;
        material.textures.push_back("movie");
        mesh->AddMaterial(std::move(material));
        node->AddMesh(mesh);
        RegisterLayer(scene, 20, node);

        WPSceneScriptHost host(&scene);
        auto registration = MakeRegistration(20,
                                             "VideoLayer",
                                             "alpha",
                                             WPSceneScriptTargetKind::Layer,
                                             WPDynamicValue::Type::Float,
                                             WPDynamicValue(1.0f));
        registration.node = node.get();
        registration.setting.script = R"(
            export function update(value) {
                const video = thisLayer.getVideoTexture();
                if (video.isPlaying()) {
                    video.pause();
                }
                video.setCurrentTime(3.5);
                return value;
            }
        )";
        assert(host.RegisterPropertyScript(std::move(registration)));
        host.Initialize();
        host.FrameBegin(0.1);

        assert(scene.videoTexturePaused["movie"]);
        assert(scene.videoTextureStopped.count("movie") == 0);
        assert(NearlyEqual(scene.videoTextureSeekRequests["movie"], 3.5));

        auto control_registration = MakeRegistration(20,
                                                     "VideoLayer",
                                                     "alpha",
                                                     WPSceneScriptTargetKind::Layer,
                                                     WPDynamicValue::Type::Float,
                                                     WPDynamicValue(1.0f));
        control_registration.node = node.get();
        control_registration.setting.script = R"(
            export function update(value) {
                const video = thisLayer.getVideoTexture();
                video.play();
                video.stop();
                video.setCurrentTime(-5);
                return value;
            }
        )";
        assert(host.RegisterPropertyScript(std::move(control_registration)));
        host.FrameBegin(0.1);
        assert(scene.videoTexturePaused["movie"]);
        assert(scene.videoTextureStopped.count("movie") == 1);
        assert(NearlyEqual(scene.videoTextureSeekRequests["movie"], 0.0));
    }

    {
        Scene scene;
        auto back_node = MakeLayerNode(31, "Back");
        auto front_node = MakeLayerNode(32, "Front");
        scene.sceneGraph->AppendChild(back_node);
        scene.sceneGraph->AppendChild(front_node);
        RegisterLayer(scene, 31, back_node, R"({"id":31,"name":"Back"})");
        RegisterLayer(scene, 32, front_node, R"({"id":32,"name":"Front"})");

        WPSceneScriptHost sort_host(&scene);
        auto sort_registration = MakeRegistration(31,
                                                  "Back",
                                                  "alpha",
                                                  WPSceneScriptTargetKind::Layer,
                                                  WPDynamicValue::Type::Float,
                                                  WPDynamicValue(1.0f));
        sort_registration.node = back_node.get();
        sort_registration.setting.script = R"(
            export function update(value) {
                const back = thisScene.getLayer('Back');
                if (!back || thisScene.getLayerCount() !== 2) return 0;
                thisScene.sortLayer(back, 1);
                return value;
            }
        )";
        assert(sort_host.RegisterPropertyScript(std::move(sort_registration)));
        sort_host.Initialize();
        sort_host.FrameBegin(0.1);
        assert(scene.layerOrder.size() == 2);
        assert(scene.layerOrder[1] == 31);

        scene.renderGraphTopologyDirty = false;
        WPSceneScriptHost layer_host(&scene);
        auto layer_registration = MakeRegistration(31,
                                                   "Back",
                                                   "alpha",
                                                   WPSceneScriptTargetKind::Layer,
                                                   WPDynamicValue::Type::Float,
                                                   WPDynamicValue(1.0f));
        layer_registration.node = back_node.get();
        layer_registration.setting.script = R"(
            export function update(value) {
                const back = thisScene.getLayer('Back');
                const front = thisScene.getLayer('Front');
                if (!back || !front || thisScene.getLayerCount() !== 2) return 0;
                if (thisScene.getInitialLayerConfig(back).name !== 'Back') return 0;
                thisScene.destroyLayer(front);
                return value;
            }
        )";
        assert(layer_host.RegisterPropertyScript(std::move(layer_registration)));
        layer_host.Initialize();
        layer_host.FrameBegin(0.1);
        layer_host.FrameBegin(0.1);

        assert(scene.layerNameToId.count("Front") == 0);
        assert(scene.renderGraphTopologyDirty);
    }

    {
        Scene scene;
        scene.shaderValueUpdater = std::make_unique<wallpaper::WPShaderValueUpdater>(&scene);
        scene.vfs = std::make_unique<wallpaper::fs::VFS>();
        auto plain_node = MakeLayerNode(33, "Plain");
        auto deferred_node = MakeLayerNode(34, "Deferred");
        scene.sceneGraph->AppendChild(plain_node);
        scene.sceneGraph->AppendChild(deferred_node);
        RegisterLayer(scene, 33, plain_node, R"({"id":33,"name":"Plain"})");
        RegisterLayer(scene, 34, deferred_node, R"({"id":34,"name":"Deferred"})");
        scene.deferredRuntimeImageLayerIds.insert(34);

        WPSceneScriptHost host(&scene);
        auto registration = MakeRegistration(33,
                                             "Plain",
                                             "alpha",
                                             WPSceneScriptTargetKind::Layer,
                                             WPDynamicValue::Type::Float,
                                             WPDynamicValue(1.0f));
        registration.node = plain_node.get();
        registration.setting.script = R"(
            export function update(value) {
                const plain = thisScene.getLayer('Plain');
                const deferred = thisScene.getLayer('Deferred');
                if (plain.getTextureAnimation() !== undefined) return 0;
                const animation = deferred.getTextureAnimation();
                if (!animation || animation.isPlaying() || animation.frameCount !== 0) return 0;
                thisScene.createLayer({ name: 'TextureAnimationProbeOk', visible: true });
                return value;
            }
        )";
        assert(host.RegisterPropertyScript(std::move(registration)));
        host.Initialize();
        host.FrameBegin(0.1);

        assert(scene.layerNameToId.count("TextureAnimationProbeOk") == 1);
    }

    {
        Scene scene;
        scene.shaderValueUpdater = std::make_unique<wallpaper::WPShaderValueUpdater>(&scene);
        scene.vfs = std::make_unique<wallpaper::fs::VFS>();

        auto root_node = MakeLayerNode(40, "Root");
        scene.sceneGraph->AppendChild(root_node);
        RegisterLayer(scene, 40, root_node, R"({"id":40,"name":"Root"})");

        WPSceneScriptHost host(&scene);
        auto registration = MakeRegistration(40,
                                             "Root",
                                             "alpha",
                                             WPSceneScriptTargetKind::Layer,
                                             WPDynamicValue::Type::Float,
                                             WPDynamicValue(1.0f));
        registration.node = root_node.get();
        registration.setting.script = R"(
            export function update(value) {
                const created = thisScene.createLayer({
                    name: 'DynamicEmpty',
                    origin: [4, 5, 0],
                    scale: [1, 1, 1],
                    angles: [0, 0, 0],
                    parent: 'Root',
                    visible: true
                });
                if (!created) return 0;
                return thisScene.getInitialLayerConfig(created).name === 'DynamicEmpty' ? value : 0;
            }
        )";
        assert(host.RegisterPropertyScript(std::move(registration)));
        host.Initialize();
        host.FrameBegin(0.1);

        const auto created_it = scene.layerNameToId.find("DynamicEmpty");
        assert(created_it != scene.layerNameToId.end());
        const int32_t created_id = created_it->second;
        assert(created_id > 0);
        assert(scene.layerNodes.count(created_id) == 1);
        assert(scene.layerNodes.at(created_id) != nullptr);
        assert(scene.layerNodes.at(created_id)->Name() == "DynamicEmpty");
        assert(scene.objectRuntimeNodes.count(created_id) == 1);
        assert(scene.initialLayerConfigJson.count(created_id) == 1);
        assert(scene.GetLayerParentBinding(created_id).parent_id == 40);
        assert(scene.IsLayerVisible(created_id));
        assert(scene.renderGraphTopologyDirty);
    }

    {
        Scene scene;
        scene.shaderValueUpdater = std::make_unique<wallpaper::WPShaderValueUpdater>(&scene);
        scene.vfs = std::make_unique<wallpaper::fs::VFS>();

        auto root_node = MakeLayerNode(50, "ShapeRoot");
        scene.sceneGraph->AppendChild(root_node);
        RegisterLayer(scene, 50, root_node, R"({"id":50,"name":"ShapeRoot"})");

        WPSceneScriptHost host(&scene);
        auto registration = MakeRegistration(50,
                                             "ShapeRoot",
                                             "alpha",
                                             WPSceneScriptTargetKind::Layer,
                                             WPDynamicValue::Type::Float,
                                             WPDynamicValue(1.0f));
        registration.node = root_node.get();
        registration.setting.script = R"(
            export function update(value) {
                const created = thisScene.createLayer({
                    name: 'DynamicShapeFallback',
                    shape: 'rectangle',
                    size: [32, 16],
                    origin: [8, 9, 0],
                    scale: [1, 1, 1],
                    angles: [0, 0, 0],
                    parent: 'ShapeRoot',
                    visible: true
                });
                if (!created) return 0;
                return thisScene.getInitialLayerConfig(created).shape === 'rectangle' ? value : 0;
            }
        )";
        assert(host.RegisterPropertyScript(std::move(registration)));
        host.Initialize();
        host.FrameBegin(0.1);

        const auto created_it = scene.layerNameToId.find("DynamicShapeFallback");
        assert(created_it != scene.layerNameToId.end());
        const int32_t created_id = created_it->second;
        assert(created_id > 0);
        assert(scene.layerNodes.count(created_id) == 1);
        assert(scene.layerNodes.at(created_id) != nullptr);
        assert(scene.layerNodes.at(created_id)->Name() == "DynamicShapeFallback");
        assert(scene.objectRuntimeNodes.count(created_id) == 1);
        assert(scene.imageLayers.count(created_id) == 0);
        assert(scene.initialLayerConfigJson.at(created_id).find("\"shape\":\"rectangle\"") !=
               std::string::npos);
        assert(scene.GetLayerParentBinding(created_id).parent_id == 50);
        assert(scene.IsLayerVisible(created_id));
        assert(scene.renderGraphTopologyDirty);
    }

    {
        Scene scene;
        scene.shaderValueUpdater = std::make_unique<wallpaper::WPShaderValueUpdater>(&scene);
        scene.vfs = std::make_unique<wallpaper::fs::VFS>();
        assert(scene.vfs->Mount(
            "/assets",
            std::make_unique<MemoryFs>(
                std::unordered_map<std::string, std::string> { { "/silent.wav", "not-a-wav" } })));
        auto sound_manager = std::make_unique<wallpaper::audio::SoundManager>();
        scene.soundManager = sound_manager.get();

        auto root_node = MakeLayerNode(60, "SoundRoot");
        scene.sceneGraph->AppendChild(root_node);
        RegisterLayer(scene, 60, root_node, R"({"id":60,"name":"SoundRoot"})");

        WPSceneScriptHost host(&scene);
        auto registration = MakeRegistration(60,
                                             "SoundRoot",
                                             "alpha",
                                             WPSceneScriptTargetKind::Layer,
                                             WPDynamicValue::Type::Float,
                                             WPDynamicValue(1.0f));
        registration.node = root_node.get();
        registration.setting.script = R"(
            export function update(value) {
                const created = thisScene.createLayer({
                    name: 'DynamicSound',
                    sound: ['silent.wav'],
                    volume: 0.25,
                    startsilent: true,
                    visible: true
                });
                if (!created) return 0;
                return thisScene.getInitialLayerConfig(created).sound[0] === 'silent.wav' ? value : 0;
            }
        )";
        assert(host.RegisterPropertyScript(std::move(registration)));
        host.Initialize();
        host.FrameBegin(0.1);

        const auto created_it = scene.layerNameToId.find("DynamicSound");
        assert(created_it != scene.layerNameToId.end());
        const int32_t created_id = created_it->second;
        assert(created_id > 0);
        assert(scene.layerNodes.count(created_id) == 0 || scene.layerNodes.at(created_id) == nullptr);
        assert(scene.objectRuntimeSoundHandles.count(created_id) == 1);
        const auto handle = scene.objectRuntimeSoundHandles.at(created_id);
        assert(handle != 0);
        assert(!sound_manager->IsPlaying(handle));
        assert(NearlyEqual(sound_manager->StreamVolume(handle), 0.25));
        assert(scene.initialLayerConfigJson.at(created_id).find("\"sound\":[\"silent.wav\"]") !=
               std::string::npos);
        assert(scene.renderGraphTopologyDirty);

        auto asset_registration = MakeRegistration(60,
                                                   "SoundRoot",
                                                   "alpha",
                                                   WPSceneScriptTargetKind::Layer,
                                                   WPDynamicValue::Type::Float,
                                                   WPDynamicValue(1.0f));
        asset_registration.node = root_node.get();
        asset_registration.setting.script = R"(
            export function update(value) {
                const created = thisScene.createLayer({ file: 'silent.wav' });
                return created && thisScene.getInitialLayerConfig(created).sound[0] === 'silent.wav'
                    ? value
                    : 0;
            }
        )";
        assert(host.RegisterPropertyScript(std::move(asset_registration)));
        host.FrameBegin(0.1);

        const auto asset_created_it = scene.layerNameToId.find("silent.wav");
        assert(asset_created_it != scene.layerNameToId.end());
        const int32_t asset_created_id = asset_created_it->second;
        assert(scene.objectRuntimeSoundHandles.count(asset_created_id) == 1);
    }

    return 0;
}
