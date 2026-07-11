#pragma once
#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>
#include "resources/WPJson.hpp"
#include "settings/WPUserProperties.hpp"
#include <nlohmann/json.hpp>

namespace wallpaper
{
namespace fs
{
class VFS;
}

namespace wpscene
{

struct WPSoundObject {
    int32_t                  id { 0 };
    bool                     locktransforms { false };
    bool                     muteineditor { false };
    bool                     nointerpolation { false };
    uint32_t                 parent { 0 };
    std::vector<int32_t>     dependencies;
    nlohmann::json           instance;
    std::string              playbackmode { "loop" };
    float                    maxtime { 10.0f };
    float                    mintime { 0.0f };
    float                    volume { 1.0f };
    bool                     visible { true };
    // Wallpaper Engine uses startsilent for script-controlled sound layers: the layer is present
    // and visible to the scripting API, but the decoder must not start producing audio until
    // layer.play() is called.  Keeping this authored flag separate from visible prevents music
    // selection scripts from being forced through an implicit autoplay/stop race.
    bool                     startsilent { false };
    bool                     blockalign { false };
    bool                     spatialization { false };
    std::string              queuemode;
    VisibleBinding           visible_binding;
    std::string              name;
    std::vector<std::string> sound;

    bool FromJson(const nlohmann::json& json, fs::VFS&) {
        GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
        GET_JSON_NAME_VALUE_NOWARN(json, "locktransforms", locktransforms);
        GET_JSON_NAME_VALUE_NOWARN(json, "muteineditor", muteineditor);
        GET_JSON_NAME_VALUE_NOWARN(json, "nointerpolation", nointerpolation);
        GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);
        ReadJsonIntArray(json, "dependencies", dependencies);
        if (json.contains("instance") && ! json.at("instance").is_null()) {
            instance = json.at("instance");
        }
        GET_JSON_NAME_VALUE(json, "volume", volume);
        GET_JSON_NAME_VALUE(json, "playbackmode", playbackmode);
        GET_JSON_NAME_VALUE_NOWARN(json, "mintime", mintime);
        GET_JSON_NAME_VALUE_NOWARN(json, "maxtime", maxtime);
        GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
        GET_JSON_NAME_VALUE_NOWARN(json, "startsilent", startsilent);
        GET_JSON_NAME_VALUE_NOWARN(json, "blockalign", blockalign);
        GET_JSON_NAME_VALUE_NOWARN(json, "spatialization", spatialization);
        GET_JSON_NAME_VALUE_NOWARN(json, "queuemode", queuemode);
        if (json.contains("visible") && json.at("visible").is_object()) {
            const auto& visible_json = json.at("visible");
            GET_JSON_NAME_VALUE_NOWARN(visible_json, "value", visible_binding.value);
            if (visible_json.contains("user") && ! visible_json.at("user").is_null()) {
                const auto& user = visible_json.at("user");
                if (user.is_string()) {
                    GET_JSON_VALUE(user, visible_binding.user.name);
                } else if (user.is_object()) {
                    GET_JSON_NAME_VALUE_NOWARN(user, "name", visible_binding.user.name);
                    GET_JSON_NAME_VALUE_NOWARN(user, "condition", visible_binding.user.condition);
                }
            }
        }
        GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
        if (! json.contains("sound") || ! json.at("sound").is_array()) {
            return false;
        }
        for (const auto& el : json.at("sound")) {
            std::string name;
            GET_JSON_VALUE(el, name);
            if (! name.empty()) sound.push_back(name);
        }
        return true;
    }
};
} // namespace wpscene
} // namespace wallpaper
