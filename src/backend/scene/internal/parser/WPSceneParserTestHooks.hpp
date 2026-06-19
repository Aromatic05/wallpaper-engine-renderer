#pragma once

#include "common/scene/Image.hpp"
#include "particle/ParticleSystem.h"
#include "wpscene/WPParticleObject.h"

namespace wallpaper
{

std::array<i32, 4> ResolvePaddedSpriteSheetResolution(const ImageHeader& texh,
                                                      const SpriteFrame& frame);

wpscene::ParticleInstanceoverride ResolveParticleSubsystemOverride(
    const wpscene::ParticleInstanceoverride& layer_override, bool is_child_subsystem);

void LoadParticleInitializers(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                              const wpscene::ParticleInstanceoverride& over);

} // namespace wallpaper
