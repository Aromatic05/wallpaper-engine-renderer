#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace wallpaper
{
struct DmabufFormatModifier {
    std::uint32_t fourcc { 0 };
    std::uint64_t modifier { 0 };

    bool operator==(const DmabufFormatModifier& other) const {
        return fourcc == other.fourcc && modifier == other.modifier;
    }
};

inline bool SupportsDmabufFormat(std::span<const DmabufFormatModifier> formats,
                                 std::uint32_t fourcc, std::uint64_t modifier) {
    for (const auto& format : formats) {
        if (format.fourcc == fourcc && format.modifier == modifier) return true;
    }
    return false;
}

inline std::vector<std::uint64_t>
CollectDmabufModifiers(std::span<const DmabufFormatModifier> formats, std::uint32_t fourcc) {
    std::vector<std::uint64_t> modifiers;
    for (const auto& format : formats) {
        if (format.fourcc != fourcc) continue;
        bool duplicate = false;
        for (const auto modifier : modifiers) {
            if (modifier == format.modifier) {
                duplicate = true;
                break;
            }
        }
        if (! duplicate) modifiers.push_back(format.modifier);
    }
    return modifiers;
}
} // namespace wallpaper
