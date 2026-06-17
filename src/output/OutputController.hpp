#pragma once

#include "common/result/Result.hpp"
#include "output/OutputSource.hpp"
#include "output/OutputTarget.hpp"

namespace wallpaper
{
class OutputController {
public:
    Result<void> bind(const OutputTarget& target, OutputSource& source) {
        m_target = target;
        return source.bind(target);
    }

    const OutputTarget& target() const { return m_target; }

private:
    OutputTarget m_target {};
};
} // namespace wallpaper
