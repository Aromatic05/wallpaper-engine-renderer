#pragma once

#include "runtime/diagnostics/Diagnostics.hpp"

namespace wallpaper
{
class DiagnosticsHub {
public:
    void merge(const DiagnosticsSnapshot& snapshot) {
        m_snapshot.entries.insert(m_snapshot.entries.end(),
                                  snapshot.entries.begin(),
                                  snapshot.entries.end());
    }

    DiagnosticsSnapshot snapshot() const { return m_snapshot; }

private:
    DiagnosticsSnapshot m_snapshot;
};
} // namespace wallpaper
