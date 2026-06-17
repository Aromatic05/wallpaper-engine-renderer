#pragma once

#include "runtime/diagnostics/Diagnostics.hpp"

namespace wallpaper
{
class DiagnosticsHub {
public:
    void append(DiagnosticSeverity severity, std::string source, std::string message) {
        m_snapshot.append(severity, std::move(source), std::move(message));
    }

    void merge(const DiagnosticsSnapshot& snapshot) {
        m_snapshot.entries.insert(m_snapshot.entries.end(),
                                  snapshot.entries.begin(),
                                  snapshot.entries.end());
    }

    void clear() { m_snapshot.entries.clear(); }

    DiagnosticsSnapshot snapshot() const { return m_snapshot; }

private:
    DiagnosticsSnapshot m_snapshot;
};
} // namespace wallpaper
