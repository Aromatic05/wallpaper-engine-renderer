#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SceneTexture.h"
#include "SceneRenderTarget.h"
#include "SceneNode.h"
#include "SceneLight.hpp"
#include "scenescript/WPSceneScriptRegistration.hpp"
#include "text/WPTextLayer.hpp"

#include "core/NoCopyMove.hpp"

namespace wallpaper
{
class ParticleSystem;
class ParticleSubSystem;
class IShaderValueUpdater;
class IImageParser;
class WPSceneScriptHost;
class SceneTextPrimitive;

namespace fs
{
class VFS;
}
namespace audio
{
class SoundManager;
using SoundHandle = uint32_t;
}
class Scene : NoCopy, NoMove {
public:
    struct LayerParentBinding {
        int32_t     parent_id { 0 };
        std::string attachment;
    };

    Scene();
    ~Scene();

    std::unordered_map<std::string, SceneTexture>      textures;
    std::unordered_map<std::string, SceneRenderTarget> renderTargets;

    std::unordered_map<std::string, std::shared_ptr<SceneCamera>> cameras;
    std::unordered_map<std::string, std::vector<std::string>>     linkedCameras;

    std::vector<std::unique_ptr<SceneLight>> lights;
    std::unordered_map<int32_t, std::shared_ptr<SceneTextPrimitive>> textPrimitives;
    std::unordered_map<int32_t, TextLayerRuntimeState> textLayers;
    std::unordered_map<int32_t, std::vector<SceneNode*>> objectRuntimeNodes;
    std::unordered_map<int32_t, std::vector<std::string>> objectRuntimeCameraNames;
    std::unordered_set<int32_t> deferredRuntimeTextLayerIds;
    std::unordered_set<std::string> dirtyRenderTargetKeys;
    std::unordered_set<int32_t> dirtyTextLayerIds;
    std::unordered_map<std::string, bool> videoTexturePaused;
    std::unordered_set<std::string>       videoTextureStopped;
    std::unordered_map<std::string, double> videoTextureSeekRequests;

    std::shared_ptr<SceneNode>           sceneGraph;
    std::unique_ptr<IShaderValueUpdater> shaderValueUpdater;
    std::unique_ptr<IImageParser>        imageParser;
    std::unique_ptr<fs::VFS>             vfs;
    std::shared_ptr<WPSceneScriptHost>   scriptHost;
    std::vector<WPSceneScriptRegistration> bindingRegistrations;
    std::vector<WPSceneScriptRegistration> scriptRegistrations;
    std::vector<WPSceneScriptRegistration> propertyAnimationRegistrations;
    UserPropertyMap                      userProperties;
    std::vector<int32_t>                 layerOrder;
    std::unordered_map<int32_t, SceneNode*> layerNodes;
    std::unordered_map<int32_t, LayerParentBinding> layerParentBindings;
    std::unordered_map<int32_t, bool>    layerLocalVisibility;
    std::unordered_map<int32_t, std::string> initialLayerConfigJson;
    std::unordered_map<std::string, int32_t> layerNameToId;
    bool renderGraphTopologyDirty { false };

    std::string scene_id { "unknown_id" };

    bool first_frame_ok { false };

    std::unordered_map<int32_t, std::vector<ParticleSubSystem*>> runtimeParticleSubsystemsByObjectId;
    std::unordered_map<std::string, int32_t>                     runtimeParticleObjectIdsByName;
    std::unordered_map<int32_t, audio::SoundHandle>              objectRuntimeSoundHandles;
    audio::SoundManager*                                         soundManager { nullptr };

    SceneMesh default_effect_mesh;

    std::unique_ptr<ParticleSystem> paritileSys;

    SceneCamera* activeCamera;

    i32                  ortho[2] { 1920, 1080 }; // w, h
    std::array<float, 3> clearColor { 1.0f, 1.0f, 1.0f };

    double elapsingTime { 0.0f }, frameTime { 0.0f };
    double textRenderScale { 1.0 };
    bool renderGraphResourcesDirty { false };
    bool renderGraphAllResourcesDirty { false };
    void MarkRenderGraphResourcesDirty() {
        renderGraphResourcesDirty = true;
        renderGraphAllResourcesDirty = true;
        dirtyRenderTargetKeys.clear();
        dirtyTextLayerIds.clear();
    }
    void MarkRenderTargetResourcesDirty(std::string render_target_key) {
        renderGraphResourcesDirty = true;
        if (!renderGraphAllResourcesDirty && !render_target_key.empty()) {
            dirtyRenderTargetKeys.insert(std::move(render_target_key));
        }
    }
    void MarkTextLayerResourcesDirty(int32_t layer_id) {
        renderGraphResourcesDirty = true;
        if (!renderGraphAllResourcesDirty && layer_id != 0) dirtyTextLayerIds.insert(layer_id);
    }
    void   PassFrameTime(double t) {
          frameTime = t;
          elapsingTime += t;
    }

    void UpdateLinkedCamera(const std::string& name) {
        if (linkedCameras.count(name) != 0) {
            auto& cams = linkedCameras.at(name);
            for (auto& cam : cams) {
                if (cameras.count(cam) != 0) {
                    cameras.at(cam)->Clone(*cameras.at(name));
                    cameras.at(cam)->Update();
                }
            }
        }
    }

    int32_t AllocateLayerId() const;
    bool    RegisterLayer(int32_t layer_id,
                          std::string name,
                          SceneNode* node,
                          std::string initial_config_json = {});
    bool    CreateRuntimeLayer(std::string name,
                                std::string initial_config_json,
                                int32_t* out_layer_id = nullptr);
    bool    DestroyLayer(int32_t layer_id);
    bool    SortLayer(int32_t layer_id, int32_t target_index);
    int32_t ResolveLayer(std::string_view name) const;
    int32_t LayerIndex(int32_t layer_id) const;
    void    SetLayerLocalVisibility(int32_t layer_id, bool visible);
    bool    GetLayerLocalVisibility(int32_t layer_id) const;
    bool    IsLayerVisible(int32_t layer_id) const;
    void    ApplyLayerVisibility(int32_t layer_id);
    void    ApplyAllLayerVisibility();
    void    SetLayerParentBinding(int32_t layer_id,
                                  int32_t parent_id,
                                  std::string attachment = {});
    LayerParentBinding GetLayerParentBinding(int32_t layer_id) const;
    void    ClearLayerParentBinding(int32_t layer_id);
    void    MarkRenderGraphTopologyDirty() {
        renderGraphTopologyDirty = true;
        renderGraphResourcesDirty = true;
    }
    void    ClearRenderGraphDirty() {
        renderGraphTopologyDirty = false;
        renderGraphResourcesDirty = false;
        renderGraphAllResourcesDirty = false;
        dirtyRenderTargetKeys.clear();
        dirtyTextLayerIds.clear();
    }
};
} // namespace wallpaper
