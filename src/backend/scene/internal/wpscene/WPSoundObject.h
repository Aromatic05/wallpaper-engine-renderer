#pragma once
#include <cstdint>
#include <unordered_map>
#include <cstdint>
#include "resources/WPJson.hpp"
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
    std::string              playbackmode { "loop" };
    float                    maxtime { 10.0f };
    float                    mintime { 0.0f };
    float                    volume { 1.0f };
    bool                     visible { true };
    bool                     startsilent { false };
    std::string              name;
    std::vector<std::string> sound;

    bool FromJson(const nlohmann::json& json, fs::VFS&) {
        GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
        GET_JSON_NAME_VALUE(json, "volume", volume);
        GET_JSON_NAME_VALUE(json, "playbackmode", playbackmode);
        GET_JSON_NAME_VALUE_NOWARN(json, "mintime", mintime);
        GET_JSON_NAME_VALUE_NOWARN(json, "maxtime", maxtime);
        GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
        GET_JSON_NAME_VALUE_NOWARN(json, "startsilent", startsilent);
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
