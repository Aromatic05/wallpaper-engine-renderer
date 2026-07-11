#pragma once

#include "particle/Particle.h"

#include <algorithm>
#include <cmath>

namespace wallpaper
{
inline float ResolveParticleRandomFrameLifetime(const Particle& particle) noexcept {
    return std::clamp(particle.random, 0.0f, std::nextafter(1.0f, 0.0f));
}
} // namespace wallpaper
