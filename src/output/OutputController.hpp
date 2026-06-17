#pragma once

#include "output/OutputSource.hpp"
#include "output/OutputTarget.hpp"
#include "runtime/backend/BackendCapabilities.hpp"

namespace wallpaper
{
class OutputController {
public:
    Result<void> bind(const OutputTarget&        target,
                      OutputSource&              source,
                      const BackendCapabilities& capabilities);

    const OutputTarget& target() const { return m_target; }

private:
    static Result<void> validate(const OutputTarget&        target,
                                 const OutputSource&        source,
                                 const BackendCapabilities& capabilities);
    static std::string targetTypeName(OutputTargetType type);
    static Result<void> bindRenderPlan(const OutputTarget& target, const OutputSource& source);

    OutputTarget m_target {};
};
} // namespace wallpaper
