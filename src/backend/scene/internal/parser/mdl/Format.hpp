#pragma once

#include "wallpaper/Result.hpp"
#include "core/Literals.hpp"

#include <cstdint>

namespace wallpaper::fs
{
class IBinaryStream;
}

namespace wallpaper
{
struct WPMdlHeader {
    i32 mdlv { 0 };
    u32 mdl_flag { 0 };
    u32 unk_a { 1 };
    u32 mesh_count { 1 };
};

inline constexpr u32 WPMDL_FLAG_POSITION = 0x00000001u;
inline constexpr u32 WPMDL_FLAG_NORMAL = 0x00000002u;
inline constexpr u32 WPMDL_FLAG_TANGENT = 0x00000004u;
inline constexpr u32 WPMDL_FLAG_UV = 0x00000008u;
inline constexpr u32 WPMDL_FLAG_UV2 = 0x00000020u;
inline constexpr u32 WPMDL_FLAG_EXTRA4 = 0x00010000u;
inline constexpr u32 WPMDL_FLAG_SKIN_BLEND = 0x00800000u;
inline constexpr u32 WPMDL_FLAG_SKIN_WEIGHT = 0x01000000u;

Result<WPMdlHeader> ParseWPMdlHeader(fs::IBinaryStream& stream);
u32 WPMdlVertexStride(u32 flags);
} // namespace wallpaper
