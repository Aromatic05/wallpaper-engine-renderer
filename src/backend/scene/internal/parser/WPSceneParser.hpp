#pragma once
#include "interface/ISceneParser.h"
#include "WPSceneScriptHost.hpp"
#include "WPUserProperties.hpp"
#include <nlohmann/json_fwd.hpp>
#include <random>

namespace wallpaper
{

class Scene;

class WPSceneParser : public ISceneParser {
public:
    WPSceneParser()  = default;
    ~WPSceneParser() = default;
    std::shared_ptr<Scene> Parse(std::string_view   scene_id,
                                 const std::string&,
                                 fs::VFS&,
                                 audio::SoundManager&,
                                 const UserPropertyMap* user_properties,
                                 double                 text_render_scale = 1.0,
                                 bool                   force_audio_loop = false);
    std::shared_ptr<Scene> Parse(std::string_view scene_id, const std::string&, fs::VFS&, audio::SoundManager&) override;
};

bool CreateDynamicSceneLayer(Scene&                                      scene,
                             const nlohmann::json&                       object_json,
                             const UserPropertyMap*                      user_properties,
                             std::vector<WPSceneScriptRegistration>*     out_binding_registrations            = nullptr,
                             std::vector<WPSceneScriptRegistration>*     out_script_registrations             = nullptr,
                             std::vector<WPSceneScriptRegistration>*     out_property_animation_registrations = nullptr,
                             std::string*                                out_initial_config_json              = nullptr,
                             int32_t*                                    out_layer_id                         = nullptr);

bool MaterializeDeferredParticleLayer(Scene& scene, int32_t layer_id,
                                      const UserPropertyMap* user_properties);
bool MaterializeDeferredTextLayer(Scene& scene, int32_t layer_id,
                                  const UserPropertyMap* user_properties);
bool MaterializeDeferredImageLayer(Scene& scene, int32_t layer_id,
                                   const UserPropertyMap* user_properties);
} // namespace wallpaper
