#pragma once
#include "resources/WPJson.hpp"
#include "WPUserSetting.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace wallpaper
{
struct WPPropertyAnimationDefinition;

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

struct WPConstantShaderValueBinding {
    // Constant shader values are authored as one JSON object that may contain a user binding,
    // a script, and a property animation at the same time. Keeping those dynamic contracts
    // together lets the script host resolve thisObject.getAnimation() against the exact same
    // material uniform registration that receives media-thumbnail and user-property updates.
    WPUserSetting                                        setting;
    std::shared_ptr<wallpaper::WPPropertyAnimationDefinition> animation;
};

class WPMaterialPass {
public:
    bool                                                FromJson(const nlohmann::json&);
    void                                                Update(const WPMaterialPass&);
    std::vector<std::string>                            textures;
    std::vector<WPUserTextureBinding>                   usertextures;
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    // Dynamic constant shader values need to survive parsing separately from their resolved
    // numeric fallback. `constantshadervalues` keeps the cold-start value used by existing uniform
    // setup, while this table preserves authored user bindings, scripts, and animations for live
    // runtime updates.
    std::unordered_map<std::string, WPConstantShaderValueBinding> constantshadervaluebindings;
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
    // Authored-state bits are deliberately stored beside the parsed material. Scene-level 3D
    // models resolve omitted render-state fields through a separate model policy, while 2D
    // image/effect materials must keep the old default strings when the source omitted a field.
    bool                                                blendingAuthored { false };
    bool                                                cullmodeAuthored { false };
    bool                                                depthtestAuthored { false };
    bool                                                depthwriteAuthored { false };
    std::vector<std::string>                            textures;
    std::vector<WPUserTextureBinding>                   usertextures;
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    // See WPMaterialPass::constantshadervaluebindings. Merged effect passes store the live
    // dynamic contracts here so already-created post-process materials can update uniforms such
    // as workshop Bloom's `strength -> u_strength` or replay thumbnail transition animations
    // without reloading the effect.
    std::unordered_map<std::string, WPConstantShaderValueBinding> constantshadervaluebindings;
    std::unordered_map<std::string, std::string>        usershadervalues;

    bool use_puppet { false };
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPMaterialPassBindItem, name, index);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPUserTextureBinding, name, type);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPMaterialPass, bind, target, textures, usertextures, combos,
                                   constantshadervalues, usershadervalues);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPMaterial, blending, shader, textures, usertextures, combos,
                                   constantshadervalues, usershadervalues);
} // namespace wpscene
} // namespace wallpaper
