#pragma once
#include <span>
#include <string>
#include <vector>
#include "Spv.hpp"

namespace wallpaper
{
namespace vulkan
{

enum class ShaderSourceLanguage
{
    HLSL
};

enum class ShaderTargetEnv
{
    VULKAN_1_0,
    VULKAN_1_1,
    VULKAN_1_2,
    VULKAN_1_3
};

struct ShaderCompUnit {
    ShaderType           stage;
    ShaderSourceLanguage source_language { ShaderSourceLanguage::HLSL };
    std::string          debug_name;
    std::string          entry_point { "main" };
    std::string src;
};

struct ShaderCompOpt {
    ShaderTargetEnv      target_env { ShaderTargetEnv::VULKAN_1_1 };
    std::vector<std::string> definitions;

    bool auto_map_locations { false };
    bool auto_map_bindings { false };
};

bool CompileAndLinkShaderUnits(std::span<const ShaderCompUnit> compUnits, const ShaderCompOpt& opt,
                               std::vector<Uni_ShaderSpv>& spvs);
bool PreprocessShaderSourceWithDxc(const std::string& src, const ShaderCompOpt& opt,
                                   std::string& out, std::string_view debug_name = {});
} // namespace vulkan
} // namespace wallpaper
