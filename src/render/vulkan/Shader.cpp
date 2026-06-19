#include "Shader.hpp"

#include <cassert>
#include "Spv.hpp"
#include "TextureCache.hpp"
#include "utils/Logging.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "core/StringHelper.hpp"
#include "utils/Sha.hpp"
#include "core/MapSet.hpp"
#include <SPIRV-Reflect/spirv_reflect.h>

#if defined(HANABI_HAS_DXC)
#include <dxc/dxcapi.h>
#endif

using namespace wallpaper;
using namespace wallpaper::vulkan;

namespace
{
inline wallpaper::ShaderType ToGeneType(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return wallpaper::ShaderType::VERTEX;
    case VK_SHADER_STAGE_GEOMETRY_BIT: return wallpaper::ShaderType::GEOMETRY;
    case VK_SHADER_STAGE_FRAGMENT_BIT: return wallpaper::ShaderType::FRAGMENT;
    default: assert(false); return wallpaper::ShaderType::VERTEX;
    }
}

inline VkShaderStageFlagBits ToVkType(wallpaper::ShaderType stage) {
    switch (stage) {
    case wallpaper::ShaderType::VERTEX: return VK_SHADER_STAGE_VERTEX_BIT;
    case wallpaper::ShaderType::GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case wallpaper::ShaderType::FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    default: assert(false); return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

inline VkFormat ToVkType(SpvReflectFormat type) { return static_cast<VkFormat>(type); }

inline VkShaderStageFlagBits ToVkType(SpvReflectShaderStageFlagBits s) {
    switch (s) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT: return VK_SHADER_STAGE_VERTEX_BIT;
    case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    default: assert(false); return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

template<typename VEC, typename FUNC>
bool EnumAllRef(VEC& vec, FUNC&& func) {
    uint count { 0 };
    auto result = func(&count, nullptr);
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    vec.resize(count);
    result = func(&count, vec.data());
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    return result == SPV_REFLECT_RESULT_SUCCESS;
}

inline const char* ShaderSourceLanguageName(ShaderSourceLanguage language) {
    switch (language) {
    case ShaderSourceLanguage::HLSL: return "hlsl";
    default: return "unknown";
    }
}

inline const wchar_t* ToDxcTargetEnv(ShaderTargetEnv target_env) {
    switch (target_env) {
    case ShaderTargetEnv::VULKAN_1_0: return L"vulkan1.0";
    case ShaderTargetEnv::VULKAN_1_1: return L"vulkan1.1";
    case ShaderTargetEnv::VULKAN_1_2: return L"vulkan1.2";
    case ShaderTargetEnv::VULKAN_1_3: return L"vulkan1.3";
    default: return L"vulkan1.1";
    }
}

inline const wchar_t* ToDxcProfile(wallpaper::ShaderType stage) {
    switch (stage) {
    case wallpaper::ShaderType::VERTEX: return L"vs_6_0";
    case wallpaper::ShaderType::FRAGMENT: return L"ps_6_0";
    case wallpaper::ShaderType::GEOMETRY: return L"gs_6_0";
    default: return L"vs_6_0";
    }
}

inline std::wstring ToWide(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

inline const char* DefaultEntryPoint(wallpaper::ShaderType stage) {
    switch (stage) {
    case wallpaper::ShaderType::VERTEX: return "main";
    case wallpaper::ShaderType::FRAGMENT: return "main";
    case wallpaper::ShaderType::GEOMETRY: return "main";
    default: return "main";
    }
}

inline std::string EffectiveEntryPoint(const ShaderCompUnit& unit) {
    return unit.entry_point.empty() ? DefaultEntryPoint(unit.stage) : unit.entry_point;
}

inline std::string_view EffectiveDebugName(const ShaderCompUnit& unit) {
    return unit.debug_name.empty() ? std::string_view { "<shader>" }
                                   : std::string_view { unit.debug_name };
}

inline std::string_view EffectiveDebugName(std::string_view debug_name) {
    return debug_name.empty() ? std::string_view { "<shader>" } : debug_name;
}

inline std::string StripHlslStructPrefix(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size()) return std::string(name);
    return std::string(name.substr(dot + 1));
}

inline std::string ReflectedDescriptorName(const SpvReflectDescriptorBinding& binding) {
    auto bind_name = std::string(binding.name).empty() && binding.type_description != nullptr &&
            binding.type_description->type_name != nullptr
        ? std::string(binding.type_description->type_name)
        : std::string(binding.name);

    constexpr std::string_view sampler_suffix { "_ww_sampler" };
    if (bind_name.size() > sampler_suffix.size() &&
        bind_name.compare(bind_name.size() - sampler_suffix.size(), sampler_suffix.size(), sampler_suffix) == 0) {
        bind_name.resize(bind_name.size() - sampler_suffix.size());
    }
    return StripHlslStructPrefix(bind_name);
}

} // namespace

bool wallpaper::vulkan::GenReflect(std::span<const std::vector<uint>> codes,
                                   std::vector<Uni_ShaderSpv>& spvs, ShaderReflected& ref) {
    spvs.clear();
    for (const auto& code : codes) {
        spv_reflect::ShaderModule spv_ref(code, SPV_REFLECT_MODULE_FLAG_NO_COPY);
        VkShaderStageFlagBits     stage = ::ToVkType(spv_ref.GetShaderStage());
        {
            Uni_ShaderSpv spv = std::make_unique<ShaderSpv>();
            spv->stage        = ::ToGeneType(stage);
            if (spv_ref.GetEntryPointCount() > 0 && spv_ref.GetEntryPointName() != nullptr) {
                spv->entry_point = spv_ref.GetEntryPointName();
            }
            spv->spirv        = code;
            spvs.emplace_back(std::move(spv));
        }
        std::vector<SpvReflectInterfaceVariable*> inputs;
        std::vector<SpvReflectDescriptorBinding*> bindings;

        bool ok = EnumAllRef(bindings, [&](auto&&... args) {
            return spv_ref.EnumerateDescriptorBindings(args...);
        });
        if (! ok) return false;

        VkDescriptorSetLayoutBinding vkbinding {};
        vkbinding.stageFlags = stage;

        for (auto pb : bindings) {
            auto& b = *pb;
            if (! b.accessed) continue;

            auto bind_name = ReflectedDescriptorName(b);

            if (exists(ref.binding_map, bind_name)) {
                auto& bind = ref.binding_map[bind_name];
                bind.stageFlags |= stage;
                continue;
            }
            if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER) {
                continue;
            }
            if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                auto& block      = b.block;
                auto  block_name = std::string(block.name).empty() ? bind_name : block.name;
                ref.blocks.push_back(ShaderReflected::Block { //.index = i,
                                                              .size       = block.size,
                                                              .name       = block_name,
                                                              .member_map = {} });
                auto& ref_block = ref.blocks.back();

                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

                for (u32 i = 0; i < block.member_count; i++) {
                    auto&                           unif = block.members[i];
                    ShaderReflected::BlockedUniform bunif {};
                    {
                        // bunif.num = GetTypeNum(type);
                        bunif.size   = unif.size;
                        bunif.offset = unif.offset;
                    }
                    ref_block.member_map[StripHlslStructPrefix(unif.name)] = bunif;
                }
            } else if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                       b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            } else {
                LOG_ERROR("unknown DescriptorBinding %d", b.descriptor_type);
                return false;
            }

            ref.binding_map[bind_name] = vkbinding;
        }

        if (stage == VK_SHADER_STAGE_VERTEX_BIT) {
            EnumAllRef(inputs, [&](auto&&... args) {
                return spv_ref.EnumerateInputVariables(args...);
            });

            for (auto pinput : inputs) {
                auto& input = *pinput;
                if (wallpaper::sstart_with(input.name, "gl_")) continue;

                if (input.location == std::numeric_limits<decltype(input.location)>::max()) {
                    LOG_ERROR("shader input %s no location", input.name);
                    return false;
                }
                ShaderReflected::Input rinput;
                rinput.location = input.location;
                rinput.format   = ::ToVkType(input.format);

                ref.input_location_map[StripHlslStructPrefix(input.name)] = rinput;
            }
        }
    }
    return true;
}

#if defined(HANABI_HAS_DXC)
static bool DxcSucceeded(HRESULT hr, std::string_view what) {
    if (SUCCEEDED(hr)) return true;
    LOG_ERROR("dxc(%.*s): HRESULT=0x%08x",
              static_cast<int>(what.size()),
              what.data(),
              static_cast<unsigned int>(hr));
    return false;
}

static std::string DxcErrorText(IDxcResult* result) {
    if (result == nullptr) return {};

    CComPtr<IDxcBlobUtf8> errors;
    if (FAILED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) ||
        errors == nullptr ||
        errors->GetStringLength() == 0) {
        return {};
    }
    return std::string(errors->GetStringPointer(), errors->GetStringLength());
}

bool wallpaper::vulkan::PreprocessShaderSourceWithDxc(const std::string& src,
                                                      const ShaderCompOpt& opt,
                                                      std::string& out,
                                                      std::string_view debug_name) {
    CComPtr<IDxcCompiler3> compiler;
    if (! DxcSucceeded(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)),
                       "create-preprocess-compiler")) {
        return false;
    }

    std::vector<std::wstring> owned_args;
    owned_args.reserve(opt.definitions.size());
    std::vector<LPCWSTR> args { L"-P" };
    for (const auto& definition : opt.definitions) {
        owned_args.emplace_back(L"-D" + ToWide(definition));
        args.push_back(owned_args.back().c_str());
    }

    DxcBuffer source {
        .Ptr      = src.data(),
        .Size     = src.size(),
        .Encoding = DXC_CP_UTF8,
    };

    CComPtr<IDxcResult> result;
    const auto          preprocess_hr =
        compiler->Compile(&source,
                          args.data(),
                          static_cast<UINT32>(args.size()),
                          nullptr,
                          IID_PPV_ARGS(&result));
    if (! DxcSucceeded(preprocess_hr, "preprocess-call")) return false;

    const auto errors = DxcErrorText(result);
    if (! errors.empty()) {
        const auto effective_debug_name = EffectiveDebugName(debug_name);
        LOG_INFO("dxc-preprocess(%.*s): %s",
                 static_cast<int>(effective_debug_name.size()),
                 effective_debug_name.data(),
                 errors.c_str());
    }

    HRESULT status = S_OK;
    if (! DxcSucceeded(result->GetStatus(&status), "preprocess-status")) return false;
    if (FAILED(status)) {
        std::string tmp_name = logToTmpfileWithSha1(src, "%s", src.c_str());
        const auto effective_debug_name = EffectiveDebugName(debug_name);
        LOG_ERROR("dxc-preprocess(%.*s): shader source is at %s",
                  static_cast<int>(effective_debug_name.size()),
                  effective_debug_name.data(),
                  tmp_name.c_str());
        return false;
    }

    CComPtr<IDxcBlobUtf8> text;
    if (! DxcSucceeded(result->GetOutput(DXC_OUT_HLSL, IID_PPV_ARGS(&text), nullptr),
                       "preprocess-output") ||
        text == nullptr) {
        return false;
    }

    out.assign(text->GetStringPointer(), text->GetStringLength());
    return true;
}

static bool CompileShaderUnitWithDxc(const ShaderCompUnit& unit,
                                     const ShaderCompOpt& opt,
                                     Uni_ShaderSpv& spv) {
    CComPtr<IDxcUtils>     utils;
    CComPtr<IDxcCompiler3> compiler;
    if (! DxcSucceeded(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)),
                       "create-utils")) {
        return false;
    }
    if (! DxcSucceeded(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)),
                       "create-compiler")) {
        return false;
    }

    CComPtr<IDxcIncludeHandler> include_handler;
    if (! DxcSucceeded(utils->CreateDefaultIncludeHandler(&include_handler),
                       "create-include-handler")) {
        return false;
    }

    const auto         entry_point = EffectiveEntryPoint(unit);
    const auto         entry_point_w = ToWide(entry_point);
    const std::wstring target_env = std::wstring(L"-fspv-target-env=") + ToDxcTargetEnv(opt.target_env);
    std::vector<std::wstring> owned_args;
    owned_args.reserve(opt.definitions.size());
    std::vector<LPCWSTR> args {
        L"-E",
        entry_point_w.c_str(),
        L"-T",
        ToDxcProfile(unit.stage),
        L"-spirv",
        target_env.c_str(),
        L"-fvk-bind-globals",
        L"0",
        L"0",
    };
    if (opt.auto_map_bindings) {
        args.push_back(L"-auto-binding-space");
        args.push_back(L"0");
    }
    for (const auto& definition : opt.definitions) {
        owned_args.emplace_back(L"-D" + ToWide(definition));
        args.push_back(owned_args.back().c_str());
    }

    DxcBuffer source {
        .Ptr      = unit.src.data(),
        .Size     = unit.src.size(),
        .Encoding = DXC_CP_UTF8,
    };

    CComPtr<IDxcResult> result;
    const auto          compile_hr =
        compiler->Compile(&source,
                          args.data(),
                          static_cast<UINT32>(args.size()),
                          include_handler,
                          IID_PPV_ARGS(&result));
    if (! DxcSucceeded(compile_hr, "compile-call")) return false;

    HRESULT status = S_OK;
    if (! DxcSucceeded(result->GetStatus(&status), "get-status")) return false;
    const auto errors = DxcErrorText(result);
    if (FAILED(status)) {
        if (! errors.empty()) {
            const auto debug_name = EffectiveDebugName(unit);
            LOG_ERROR("dxc(%.*s): %s",
                      static_cast<int>(debug_name.size()),
                      debug_name.data(),
                      errors.c_str());
        }
        std::string tmp_name = logToTmpfileWithSha1(unit.src, "%s", unit.src.c_str());
        const auto debug_name = EffectiveDebugName(unit);
        LOG_ERROR("dxc(%.*s): shader source is at %s",
                  static_cast<int>(debug_name.size()),
                  debug_name.data(),
                  tmp_name.c_str());
        return false;
    }
    if (! errors.empty()) {
        const auto debug_name = EffectiveDebugName(unit);
        LOG_WARN("dxc(%.*s): %s",
                 static_cast<int>(debug_name.size()),
                 debug_name.data(),
                 errors.c_str());
    }

    CComPtr<IDxcBlob> object;
    if (! DxcSucceeded(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr),
                       "get-object") ||
        object == nullptr) {
        return false;
    }

    const auto byte_size = object->GetBufferSize();
    if (byte_size % sizeof(unsigned int) != 0) {
        const auto debug_name = EffectiveDebugName(unit);
        LOG_ERROR("dxc(%.*s): SPIR-V byte size %zu is not word-aligned",
                  static_cast<int>(debug_name.size()),
                  debug_name.data(),
                  byte_size);
        return false;
    }

    spv              = std::make_unique<ShaderSpv>();
    spv->stage       = unit.stage;
    spv->entry_point = entry_point;
    const auto words = byte_size / sizeof(unsigned int);
    spv->spirv.resize(words);
    std::memcpy(spv->spirv.data(), object->GetBufferPointer(), byte_size);
    return true;
}
#else
bool wallpaper::vulkan::PreprocessShaderSourceWithDxc(const std::string& src,
                                                      const ShaderCompOpt& opt,
                                                      std::string& out,
                                                      std::string_view debug_name) {
    (void)src;
    (void)opt;
    (void)out;
    const auto effective_debug_name = EffectiveDebugName(debug_name);
    LOG_ERROR("dxc-preprocess(%.*s): Hanabi was built without a staged DXC SDK",
              static_cast<int>(effective_debug_name.size()),
              effective_debug_name.data());
    return false;
}
#endif

static bool CompileShaderUnitsWithDxc(std::span<const ShaderCompUnit> compUnits,
                                      const ShaderCompOpt& opt,
                                      std::vector<Uni_ShaderSpv>& spvs) {
#if defined(HANABI_HAS_DXC)
    spvs.clear();
    spvs.reserve(compUnits.size());
    for (const auto& unit : compUnits) {
        Uni_ShaderSpv spv;
        if (! CompileShaderUnitWithDxc(unit, opt, spv)) return false;
        spvs.emplace_back(std::move(spv));
    }
    return true;
#else
    (void)compUnits;
    (void)opt;
    (void)spvs;
    LOG_ERROR("DXC compiler requested but Hanabi was built without a staged DXC SDK");
    return false;
#endif
}

bool wallpaper::vulkan::CompileAndLinkShaderUnits(std::span<const ShaderCompUnit> compUnits,
                                                  const ShaderCompOpt& opt,
                                                  std::vector<Uni_ShaderSpv>& spvs) {
    LOG_VERBOSE("ShaderCompiler: units=%zu", compUnits.size());
    for (const auto& unit : compUnits) {
        LOG_VERBOSE("ShaderCompiler: unit stage=%d language=%s name='%s' bytes=%zu",
                 static_cast<int>(unit.stage),
                 ShaderSourceLanguageName(unit.source_language),
                 unit.debug_name.empty() ? "<shader>" : unit.debug_name.c_str(),
                 unit.src.size());
    }

    LOG_VERBOSE("ShaderCompiler: selected=dxc reason=renderer-dxc-only");
    return CompileShaderUnitsWithDxc(compUnits, opt, spvs);
}
