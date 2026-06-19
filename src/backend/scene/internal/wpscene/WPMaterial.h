#pragma once
#include "resources/WPJson.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace wallpaper
{
namespace wpscene
{

class WPMaterialPassBindItem {
public:
    bool        FromJson(const nlohmann::json&);
    std::string name;
    int32_t     index;
};

class WPUserTextureBinding {
public:
    bool        FromJson(const nlohmann::json&);
    bool        empty() const noexcept { return name.empty(); }
    std::string name;
    std::string type;
};

class WPMaterialPass {
public:
    bool                                                FromJson(const nlohmann::json&);
    void                                                Update(const WPMaterialPass&);
    std::vector<std::string>                            textures;
    std::vector<WPUserTextureBinding>                   usertextures;
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    std::unordered_map<std::string, std::string>        usershadervalues;
    std::string                                         target;
    std::vector<WPMaterialPassBindItem>                 bind;
};

class WPMaterial {
public:
    bool                                                FromJson(const nlohmann::json&);
    void                                                MergePass(const WPMaterialPass&);
    std::string                                         blending { "translucent" };
    std::string                                         cullmode { "nocull" };
    std::string                                         shader;
    std::string                                         depthtest { "disabled" };
    std::string                                         depthwrite { "disabled" };
    std::vector<std::string>                            textures;
    std::vector<WPUserTextureBinding>                   usertextures;
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    std::unordered_map<std::string, std::string>        usershadervalues;

    bool use_puppet { false };
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPMaterialPassBindItem, name, index);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPUserTextureBinding, name, type);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPMaterialPass, bind, target, textures, usertextures, combos,
                                   constantshadervalues, usershadervalues);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPMaterial, blending, shader, textures, combos,
                                   usertextures, constantshadervalues, usershadervalues);
} // namespace wpscene
} // namespace wallpaper
