#pragma once
#include "interface/ISceneParser.h"
#include "settings/WPUserProperties.hpp"
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace wallpaper
{

class Scene;
class SceneNode;
struct WPSceneScriptRegistration;

class WPSceneParser : public ISceneParser {
public:
    WPSceneParser()  = default;
    ~WPSceneParser() = default;
    std::shared_ptr<Scene> Parse(std::string_view        scene_id,
                                 const std::string&      source,
                                 fs::VFS&                vfs,
                                 audio::SoundManager&    sound_manager,
                                 const UserPropertyMap*  user_properties,
                                 double                  text_render_scale = 1.0);
    std::shared_ptr<Scene> Parse(std::string_view scene_id, const std::string&, fs::VFS&, audio::SoundManager&) override;
};

void RegisterSceneScriptBindingsForTest(Scene& scene, const nlohmann::json& scene_json,
                                        SceneNode* node);
bool CreateDynamicSceneLayer(
    Scene& scene,
    const nlohmann::json& object_json,
    const UserPropertyMap* user_properties,
    std::vector<WPSceneScriptRegistration>* out_binding_registrations = nullptr,
    std::vector<WPSceneScriptRegistration>* out_script_registrations = nullptr,
    std::vector<WPSceneScriptRegistration>* out_property_animation_registrations = nullptr,
    std::string* out_initial_config_json = nullptr,
    int32_t* out_layer_id = nullptr);
} // namespace wallpaper
