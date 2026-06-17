#pragma once

#include <memory>

namespace wallpaper
{
enum class OutputTargetBindingKind
{
    Surface,
    Offscreen,
    WESceneVulkan
};

class OutputTargetBinding {
public:
    virtual ~OutputTargetBinding() = default;

    virtual OutputTargetBindingKind kind() const = 0;
};

using OutputTargetBindingPtr = std::shared_ptr<OutputTargetBinding>;
} // namespace wallpaper
