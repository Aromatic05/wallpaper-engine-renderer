#include "Override.hpp"

namespace wallpaper
{
wpscene::ParticleInstanceoverride ResolveParticleSubsystemOverride(
    const wpscene::ParticleInstanceoverride& layer_override,
    bool is_child_subsystem) {
    if (! is_child_subsystem) return layer_override;

    // A child preset is a separately authored particle definition. Wallpaper Engine propagates
    // only the placed layer's opacity/tint into it; child emission, lifetime, size, velocity and
    // control-point contracts remain owned by the child asset.
    wpscene::ParticleInstanceoverride child_override;
    child_override.enabled    = layer_override.enabled;
    child_override.alpha      = layer_override.alpha;
    child_override.overColor  = layer_override.overColor;
    child_override.overColorn = layer_override.overColorn;
    child_override.color      = layer_override.color;
    child_override.colorn     = layer_override.colorn;
    return child_override;
}
} // namespace wallpaper
