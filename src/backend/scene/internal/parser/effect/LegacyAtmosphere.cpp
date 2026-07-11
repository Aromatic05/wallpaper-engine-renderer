#include "LegacyAtmosphere.hpp"

#include "SpecTexs.hpp"
#include "parser/WPShaderParser.hpp"
#include "wpscene/WPMaterial.h"

#include <array>
#include <string>

namespace wallpaper
{
namespace
{
constexpr std::string_view kLegacyAtmosphereShader {
    "workshop/2839476907/effects/atmosphere"
};

void ReplaceAllInPlace(std::string& source, std::string_view needle,
                       std::string_view replacement) {
    for (size_t pos = 0; (pos = source.find(needle, pos)) != std::string::npos;
         pos += replacement.size()) {
        source.replace(pos, needle.size(), replacement);
    }
}
} // namespace

bool IsLegacyAtmosphereMaterial(const wpscene::WPMaterial& material) {
    return material.shader == kLegacyAtmosphereShader;
}

void ApplyLegacyAtmosphereUniformAliases(const wpscene::WPMaterial& material,
                                         WPShaderInfo& shader_info) {
    if (!IsLegacyAtmosphereMaterial(material)) return;

    shader_info.baseConstSvs[std::string(G_VIEWFORWARD)] =
        std::array<float, 3> { 0.0f, 0.0f, 1.0f };

    auto prefer_legacy = [&](std::string_view legacy, std::string_view current) {
        if (material.constantshadervalues.count(std::string(legacy)) == 0) return;
        const auto current_it = shader_info.alias.find(std::string(current));
        if (current_it == shader_info.alias.end()) return;
        shader_info.alias[std::string(legacy)] = current_it->second;
        shader_info.alias.erase(current_it);
    };

    prefer_legacy("Planet position", "Position");
    prefer_legacy("Planet radius", "Planet size");
    prefer_legacy("Atmosphere radius", "Atmosphere size");
    prefer_legacy("Thickness", "Density falloff");
    prefer_legacy("Color", "Light color");
    prefer_legacy("Intensity", "Brightness");
}

void ApplyLegacyAtmosphereLightCombo(const wpscene::WPMaterial& material,
                                     WPShaderInfo& shader_info) {
    if (!IsLegacyAtmosphereMaterial(material)) return;
    if (shader_info.combos.count("LIGHT_INDEX") == 0 ||
        material.combos.count("LIGHT_INDEX") != 0 ||
        material.combos.count("LIGHT1") == 0) {
        return;
    }
    shader_info.combos["LIGHT_INDEX"] = "4";
}

void ApplyLegacyAtmosphereShaderCompat(const wpscene::WPMaterial& material,
                                       std::span<WPShaderUnit> units) {
    if (!IsLegacyAtmosphereMaterial(material)) return;

    for (auto& unit : units) {
        if (unit.stage != ShaderType::FRAGMENT) continue;
        ReplaceAllInPlace(unit.src,
                          "float pointDensity, opticalDepth;",
                          "float pointDensity = 0.0, opticalDepth = 0.0;");
        ReplaceAllInPlace(
            unit.src,
            "float localDensity, cameraOpticalDepth, sunRayLength, sunOpticalDepth, "
            "lightInstensity = 1.0;",
            "float localDensity = 0.0, cameraOpticalDepth = 0.0, sunRayLength = 0.0, "
            "sunOpticalDepth = 0.0, lightInstensity = 1.0;");
    }
}

bool IsLegacyAtmosphereShadowValue(const wpscene::WPMaterial& material,
                                   std::string_view material_value_name) {
    if (!IsLegacyAtmosphereMaterial(material)) return false;

    static constexpr std::array shadow_values {
        std::string_view("Position"),
        std::string_view("Planet size"),
        std::string_view("Atmosphere size"),
        std::string_view("Density falloff"),
        std::string_view("Light color"),
        std::string_view("Brightness"),
        std::string_view("Radius"),
    };
    for (const auto value : shadow_values) {
        if (material_value_name == value) return true;
    }
    return false;
}

} // namespace wallpaper
