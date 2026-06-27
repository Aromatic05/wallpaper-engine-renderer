#include "Logging.h"
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <string_view>
#include <mutex>

#include "Sha.hpp"

constexpr const char* level_names[] = { "INFO", "WARN", "ERROR" };
constexpr const char* level_fmt[]   = { "%-5s", "%-5s %s:%d ", "%-5s %s:%d " };

bool WallpaperVerboseLogEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("HANABI_VERBOSE_LOG");
        if (value == nullptr) return false;
        const std::string_view text { value };
        return !(text.empty() || text == "0" || text == "false" || text == "FALSE" ||
                 text == "off" || text == "OFF");
    }();
    return enabled;
}

namespace
{
std::FILE* DebugLogFile() {
    static std::FILE* file = []() -> std::FILE* {
        const char* path = std::getenv("HANABI_LOG_FILE");
        if (path == nullptr || path[0] == '\0') return nullptr;
        return std::fopen(path, "a");
    }();
    return file;
}

std::FILE* LogOutput() {
    if (auto* file = DebugLogFile(); file != nullptr) return file;
    return stderr;
}

bool EnvFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) return false;
    const std::string_view text { value };
    return !(text.empty() || text == "0" || text == "false" || text == "FALSE" ||
             text == "off" || text == "OFF");
}

const std::optional<std::unordered_set<int32_t>>& DebugLayerFilter() {
    static const std::optional<std::unordered_set<int32_t>> filter = []() -> std::optional<std::unordered_set<int32_t>> {
        const char* value = std::getenv("HANABI_DEBUG_LAYERS");
        if (value == nullptr || value[0] == '\0') return std::nullopt;

        std::unordered_set<int32_t> layers;
        std::stringstream           ss(value);
        std::string                 token;
        while (std::getline(ss, token, ',')) {
            if (token.empty()) continue;
            try {
                layers.insert(std::stoi(token));
            } catch (...) {
            }
        }
        return layers;
    }();
    return filter;
}

const std::optional<std::unordered_set<std::string>>& DebugModuleFilter() {
    static const std::optional<std::unordered_set<std::string>> filter = []() -> std::optional<std::unordered_set<std::string>> {
        const char* value = std::getenv("HANABI_DEBUG_MODULES");
        if (value == nullptr || value[0] == '\0') return std::nullopt;

        std::unordered_set<std::string> modules;
        std::stringstream               ss(value);
        std::string                     token;
        while (std::getline(ss, token, ',')) {
            if (token.empty()) continue;
            modules.insert(token);
        }
        return modules;
    }();
    return filter;
}
} // namespace

bool WallpaperDebugLogEnabled() { return EnvFlagEnabled("HANABI_DEBUG_LOG"); }

bool WallpaperDebugLayerEnabled(int32_t layer_id) {
    if (!WallpaperDebugLogEnabled()) return false;
    const auto& filter = DebugLayerFilter();
    if (!filter.has_value()) return true;
    return filter->count(layer_id) != 0;
}

bool WallpaperDebugModuleEnabled(const char* module) {
    if (!WallpaperDebugLogEnabled()) return false;
    const auto& filter = DebugModuleFilter();
    if (!filter.has_value()) return true;
    if (module == nullptr) return false;
    return filter->count(module) != 0;
}

void WallpaperLog(int level, const char* file, int line, const char* fmt, ...) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);

    auto* out = LogOutput();
    std::va_list args;
    std::fprintf(out, level_fmt[level], level_names[level], file, line);
    {
        va_start(args, fmt);
        std::vfprintf(out, fmt, args);
        va_end(args);
    }
    std::fprintf(out, "\n");
    std::fflush(out);
}

std::string logToTmpfileWithSha1(std::span<const char> in, const char* fmt, ...) {
    std::va_list          args;
    std::string           name   = utils::genSha1(in);
    std::filesystem::path fspath = std::filesystem::temp_directory_path() / name;
    std::string           path   = fspath.native();
    auto*                 file   = std::fopen(path.c_str(), "w+");
    {
        va_start(args, fmt);
        std::vfprintf(file, fmt, args);
        va_end(args);
    }
    std::fprintf(file, "\n");
    std::fclose(file);
    return path;
}
