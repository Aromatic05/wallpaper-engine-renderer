#include "WPMaterial.h"
#include "WPPropertyAnimation.hpp"

using namespace wallpaper::wpscene;

namespace
{

wallpaper::WPDynamicValue::Type ShaderValueTypeForConstantVector(const std::vector<float>& value) {
    switch (value.size()) {
        case 1:
            return wallpaper::WPDynamicValue::Type::Float;
        case 2:
            return wallpaper::WPDynamicValue::Type::Float2;
        case 3:
            return wallpaper::WPDynamicValue::Type::Float3;
        case 4:
            return wallpaper::WPDynamicValue::Type::Float4;
        default:
            return wallpaper::WPDynamicValue::Type::FloatVector;
    }
}

void StoreDynamicConstantShaderValue(
    std::unordered_map<std::string, WPConstantShaderValueBinding>& bindings,
    const std::string&                                             name,
    const nlohmann::json&                                          value_json,
    const std::vector<float>&                                      resolved_value) {
    if (! value_json.is_object()) return;

    const auto value_type = ShaderValueTypeForConstantVector(resolved_value);

    wallpaper::WPUserSetting setting;
    if (! wallpaper::ParseUserSetting(value_json, setting, value_type)) {
        return;
    }

    wallpaper::WPPropertyAnimationDefinition animation_definition;
    const bool has_animation =
        wallpaper::ParsePropertyAnimationDefinition(value_json, value_type, animation_definition);
    if (! setting.hasUserBinding() && ! setting.hasScript() && ! has_animation) return;

    // Constant shader values can be authored as plain numbers, arrays, user bindings, or scripts.
    // The resolved numeric value is already stored in `constantshadervalues`; keeping this sidecar
    // binding records the live runtime contract that would otherwise disappear after parse. That
    // includes property animations because media-thumbnail transition scripts call
    // thisObject.getAnimation().play() on the same material-uniform proxy.
    WPConstantShaderValueBinding binding;
    binding.setting = std::move(setting);
    if (has_animation) {
        binding.animation =
            std::make_shared<wallpaper::WPPropertyAnimationDefinition>(std::move(animation_definition));
    }
    bindings[name] = std::move(binding);
}

} // namespace

bool WPMaterialPassBindItem::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "name", name);
    GET_JSON_NAME_VALUE(json, "index", index);
    return true;
}

bool WPUserTextureBinding::FromJson(const nlohmann::json& json) {
    if (json.is_string()) {
        GET_JSON_VALUE(json, name);
        return ! name.empty();
    }

    if (! json.is_object()) return false;

    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "type", type);
    return ! name.empty();
}


void WPMaterialPass::Update(const WPMaterialPass& p) {
    int32_t i = -1;
    for(const auto& el:p.textures) {
        i++;
        if(p.textures.size() > textures.size())
            textures.resize(p.textures.size());
        if(!el.empty()) {
            textures[i] = el;
        }
    }
    i = -1;
    for (const auto& el : p.usertextures) {
        i++;
        if (p.usertextures.size() > usertextures.size())
            usertextures.resize(p.usertextures.size());
        if (! el.empty()) {
            usertextures[i] = el;
        }
    }
    for(const auto& el:p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    for (const auto& el : p.constantshadervaluebindings) {
        constantshadervaluebindings[el.first] = el.second;
    }
    for(const auto& el:p.usershadervalues) {
        usershadervalues[el.first] = el.second;
    }
    for(const auto& el:p.combos) {
        combos[el.first] = el.second;
    }
}

void WPMaterial::MergePass(const WPMaterialPass& p) {
    int32_t i = -1;
    for(const auto& el:p.textures) {
        i++;
        if(p.textures.size() > textures.size())
            textures.resize(p.textures.size());
        if(!el.empty()) {
            textures[i] = el;
        }
    }
    i = -1;
    for (const auto& el : p.usertextures) {
        i++;
        if (p.usertextures.size() > usertextures.size())
            usertextures.resize(p.usertextures.size());
        if (! el.empty()) {
            usertextures[i] = el;
        }
    }
    for(const auto& el:p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    for (const auto& el : p.constantshadervaluebindings) {
        constantshadervaluebindings[el.first] = el.second;
    }
    for(const auto& el:p.usershadervalues) {
        usershadervalues[el.first] = el.second;
    }
    for(const auto& el:p.combos) {
        combos[el.first] = el.second;
    }
}

bool WPMaterialPass::FromJson(const nlohmann::json& json) {
    if(json.contains("textures")) {
        for(const auto& jT:json.at("textures")) {
            std::string tex;
            if(!jT.is_null())
                GET_JSON_VALUE(jT, tex);
            textures.push_back(tex);
        }
    }
    if (json.contains("usertextures")) {
        for (const auto& jT : json.at("usertextures")) {
            WPUserTextureBinding binding;
            if (! jT.is_null())
                binding.FromJson(jT);
            usertextures.push_back(binding);
        }
    }
    if(json.contains("constantshadervalues")) {
        for(const auto& jC:json.at("constantshadervalues").items()) {
            std::string name;
            std::vector<float> value;
            GET_JSON_VALUE(jC.key(), name);
            GET_JSON_VALUE(jC.value(), value);
            constantshadervalues[name] = value;
            StoreDynamicConstantShaderValue(
                constantshadervaluebindings, name, jC.value(), value);
        }
    }
    if(json.contains("usershadervalues")) {
        for(const auto& jC:json.at("usershadervalues").items()) {
            std::string name;
            std::string value;
            GET_JSON_VALUE(jC.key(), name);
            GET_JSON_VALUE(jC.value(), value);
            usershadervalues[name] = value;
        }
    }
    if(json.contains("combos")) {
        for(const auto& jC:json.at("combos").items()) {
            std::string name;
            int32_t value;
            GET_JSON_VALUE(jC.key(), name);
            GET_JSON_VALUE(jC.value(), value);
            combos[name] = value;
        }
    }
    GET_JSON_NAME_VALUE_NOWARN(json, "target", target);
    if(json.contains("bind")) {
        for(const auto& jB:json.at("bind")) {
            WPMaterialPassBindItem bindItem;
            bindItem.FromJson(jB);
            bind.push_back(bindItem);
        }
    }
    return true;
}

bool WPMaterial::FromJson(const nlohmann::json& json) {
    if(!json.contains("passes") || json.at("passes").size() == 0) {
        LOG_ERROR("material no data");
        return false;
    }
    const auto jContent = json.at("passes").at(0);
    if(!jContent.contains("shader")) {
        LOG_ERROR("material no shader");
        return false;
    }
    blendingAuthored = jContent.contains("blending");
    cullmodeAuthored = jContent.contains("cullmode");
    depthtestAuthored = jContent.contains("depthtest");
    depthwriteAuthored = jContent.contains("depthwrite");
	GET_JSON_NAME_VALUE(jContent, "blending", blending);
	GET_JSON_NAME_VALUE(jContent, "cullmode", cullmode);
	GET_JSON_NAME_VALUE(jContent, "depthtest", depthtest);
	GET_JSON_NAME_VALUE(jContent, "depthwrite", depthwrite);
	GET_JSON_NAME_VALUE(jContent, "shader", shader);
    if(jContent.contains("textures")) {
        for(const auto& jT:jContent.at("textures")) {
            std::string tex;
            if(!jT.is_null())
                GET_JSON_VALUE(jT, tex);
            textures.push_back(tex);
        }
    }
    if (jContent.contains("usertextures")) {
        for (const auto& jT : jContent.at("usertextures")) {
            WPUserTextureBinding binding;
            if (! jT.is_null())
                binding.FromJson(jT);
            usertextures.push_back(binding);
        }
    }
    if(jContent.contains("constantshadervalues")) {
        for(const auto& jC:jContent.at("constantshadervalues").items()) {
            std::string name;
            std::vector<float> value;
            GET_JSON_VALUE(jC.key(), name);
            GET_JSON_VALUE(jC.value(), value);
            constantshadervalues[name] = value;
            StoreDynamicConstantShaderValue(
                constantshadervaluebindings, name, jC.value(), value);
        }
    }
    if(jContent.contains("usershadervalues")) {
        for(const auto& jC:jContent.at("usershadervalues").items()) {
            std::string name;
            std::string value;
            GET_JSON_VALUE(jC.key(), name);
            GET_JSON_VALUE(jC.value(), value);
            usershadervalues[name] = value;
        }
    }
    if(jContent.contains("combos")) {
        for(const auto& jC:jContent.at("combos").items()) {
            std::string name;
            int32_t value;
            GET_JSON_VALUE(jC.key(), name);
            GET_JSON_VALUE(jC.value(), value);
            combos[name] = value;
        }
    }
    return true;
}
