#pragma once

#include "common/scene/Image.hpp"
#include "particle/ParticleSystem.h"
#include "WPParticleParser.hpp"
#include "wpscene/WPParticleObject.h"
#include "particle/Override.hpp"

std::array<wallpaper::i32, 4> ResolvePaddedSpriteSheetResolution(
    const wallpaper::ImageHeader& texh,
    const wallpaper::SpriteFrame& frame);

namespace wallpaper
{

using ::ResolvePaddedSpriteSheetResolution;


inline void LoadParticleInitializers(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                                     const wpscene::ParticleInstanceoverride& over) {
    const bool replaces_color = over.enabled && (over.overColor || over.overColorn);
    for (const auto& ini : wp.initializers) {
        if (replaces_color && ini.contains("name") && ini.at("name").is_string() &&
            ini.at("name").get<std::string>() == "colorrandom") {
            continue;
        }
        pSys.AddInitializer(WPParticleParser::genParticleInitOp(ini));
    }
    if (over.enabled) pSys.AddInitializer(WPParticleParser::genOverrideInitOp(over));
}

} // namespace wallpaper
