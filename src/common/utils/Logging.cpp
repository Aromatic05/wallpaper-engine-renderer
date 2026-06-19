#include "Logging.h"
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string_view>

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

void WallpaperLog(int level, const char* file, int line, const char* fmt, ...) {
    std::va_list args;
    std::fprintf(stderr, level_fmt[level], level_names[level], file, line);
    {
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
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
