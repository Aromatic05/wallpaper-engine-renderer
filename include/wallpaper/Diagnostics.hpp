#pragma once

#include <string>
#include <vector>

namespace wallpaper
{
enum class DiagnosticSeverity
{
    Info,
    Warning,
    Error
};

struct DiagnosticEntry {
    DiagnosticSeverity severity { DiagnosticSeverity::Info };
    std::string        source;
    std::string        message;
};

struct DiagnosticsSnapshot {
    std::vector<DiagnosticEntry> entries;

    void append(DiagnosticSeverity severity, std::string source, std::string message) {
        entries.push_back(DiagnosticEntry { severity, std::move(source), std::move(message) });
    }
};
} // namespace wallpaper
