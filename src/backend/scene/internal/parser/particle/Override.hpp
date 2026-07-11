#pragma once

#include "wpscene/WPParticleObject.h"

namespace wallpaper
{
wpscene::ParticleInstanceoverride ResolveParticleSubsystemOverride(
    const wpscene::ParticleInstanceoverride& layer_override,
    bool is_child_subsystem);
} // namespace wallpaper
