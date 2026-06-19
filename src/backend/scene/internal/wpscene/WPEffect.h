#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "WPUserProperties.hpp"
#include "wpscene/WPMaterial.h"

namespace wallpaper
{
namespace fs
{
class VFS;
}

namespace wpscene
{

class WPEffectCommand {
public:
    bool        FromJson(const nlohmann::json&);
    std::string command;
    std::string target;
    std::string source;

    int32_t afterpos { 0 };
};

class WPEffectFbo {
public:
    bool                   FromJson(const nlohmann::json&);
    std::array<int32_t, 2> ResolveSize(std::array<float, 2> source_size) const;
    std::string            name;
    std::string            format;
    uint32_t               scale { 1 };
    uint32_t               fit { 0 };
};

class WPImageEffect {
private:
    static const std::unordered_set<std::string> BLACKLISTED_WORKSHOP_EFFECTS;
    bool IsEffectBlacklisted(const std::string& filePath);

public:
    bool FromJson(const nlohmann::json&, fs::VFS& vfs);
    bool FromFileJson(const nlohmann::json&, fs::VFS& vfs);
    bool HasEnabledCombo(const std::string& combo_name) const;
    std::unordered_set<std::string> FeedbackFboNames() const;

    int32_t                     id { 0 };
    std::string                 name;
    bool                        visible { true };
    nlohmann::json              visible_json;
    VisibleBinding              visible_binding;
    int32_t                     version { 0 };
    std::vector<WPMaterial>     materials;
    std::vector<WPMaterialPass> passes;
    std::vector<WPEffectCommand> commands;
    std::vector<WPEffectFbo>    fbos;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPEffectFbo, name, scale, fit);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPImageEffect, name, visible, passes, fbos, materials);

} // namespace wpscene
} // namespace wallpaper
