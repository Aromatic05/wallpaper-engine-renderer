#pragma once
#include "interface/ISceneParser.h"
#include <random>

#include <nlohmann/json_fwd.hpp>

namespace wallpaper
{

class Scene;
class SceneNode;

class WPSceneParser : public ISceneParser {
public:
    WPSceneParser()  = default;
    ~WPSceneParser() = default;
    std::shared_ptr<Scene> Parse(std::string_view scene_id, const std::string&, fs::VFS&, audio::SoundManager&) override;
};

void RegisterSceneScriptBindingsForTest(Scene& scene, const nlohmann::json& scene_json,
                                        SceneNode* node);
} // namespace wallpaper
