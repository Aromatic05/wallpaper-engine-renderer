#pragma once

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

constexpr std::string_view ARG_PROJECT_JSON = "<project.json>";

struct Args {
    std::string assets_uri;
    std::string uri;
    std::string cache_path;
    std::string user_properties_path;
    std::string graphviz_path;
    int32_t       fps    { 15 };
    int32_t       width  { 1280 };
    int32_t       height { 720 };
    std::uint32_t msaa_samples { 1 };
    float         mouse_x { 0.0f };
    float         mouse_y { 0.0f };
    bool          force_shm { false };
    bool          enable_valid_layer { false };
    bool          fixed_mouse_position { false };
    bool          print_diagnostics { false };
};

inline void printUsage(const char* prog) {
    // printed by parseArgs when needed
    (void)prog;
}

inline bool parsePositiveUint32(std::string_view value, std::uint32_t& result) {
    if (value.empty()) return false;
    std::uint32_t parsed { 0 };
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc {} || end != value.data() + value.size() || parsed == 0) {
        return false;
    }
    result = parsed;
    return true;
}

inline bool parseNormalizedFloat(std::string_view value, float& result) {
    if (value.empty()) return false;
    std::string text(value);
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(text.c_str(), &end);
    if (errno != 0 || end != text.c_str() + text.size() || parsed < 0.0f || parsed > 1.0f) {
        return false;
    }
    result = parsed;
    return true;
}

inline bool parseMousePosition(std::string_view value, float& x, float& y) {
    const auto separator = value.find(',');
    if (separator == std::string_view::npos || value.find(',', separator + 1) != std::string_view::npos) {
        return false;
    }
    return parseNormalizedFloat(value.substr(0, separator), x)
           && parseNormalizedFloat(value.substr(separator + 1), y);
}

inline std::string quoteJsonString(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': quoted += "\\\""; break;
        case '\\': quoted += "\\\\"; break;
        case '\b': quoted += "\\b"; break;
        case '\f': quoted += "\\f"; break;
        case '\n': quoted += "\\n"; break;
        case '\r': quoted += "\\r"; break;
        case '\t': quoted += "\\t"; break;
        default:
            if (byte < 0x20u) {
                constexpr char hex[] = "0123456789abcdef";
                quoted += "\\u00";
                quoted.push_back(hex[(byte >> 4u) & 0x0fu]);
                quoted.push_back(hex[byte & 0x0fu]);
            } else {
                quoted.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    quoted.push_back('"');
    return quoted;
}

inline std::string buildSourceOptionsJson(const Args& args,
                                          std::string_view user_properties_json) {
    if (user_properties_json.empty() && args.graphviz_path.empty()) return {};

    std::string options = "{\"version\":1,\"scene\":{";
    bool first = true;
    if (! user_properties_json.empty()) {
        options += "\"userProperties\":";
        options.append(user_properties_json);
        first = false;
    }
    if (! args.graphviz_path.empty()) {
        if (! first) options.push_back(',');
        options += "\"graphviz\":{\"enabled\":true,\"path\":";
        options += quoteJsonString(args.graphviz_path);
        options += '}';
    }
    options += "}}";
    return options;
}

// Minimal hand-rolled parser. Avoids the third_party/argparse header
// since the C ABI demo only takes a handful of flags.
inline bool parseArgs(int argc, char** argv, Args& args, std::string& err) {
    auto needValue = [&](int& i, std::string& out) -> bool {
        if (i + 1 >= argc) {
            err = std::string(argv[i]) + " requires a value";
            return false;
        }
        out = argv[++i];
        return true;
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            return false;
        } else if (a == "--shm") {
            args.force_shm = true;
        } else if (a == "--valid-layer") {
            args.enable_valid_layer = true;
        } else if (a == "--diagnostics") {
            args.print_diagnostics = true;
        } else if (a == "--user-properties") {
            if (! needValue(i, args.user_properties_path)) return false;
        } else if (a == "--graphviz") {
            if (! needValue(i, args.graphviz_path)) return false;
        } else if (a == "--mouse-position") {
            std::string v;
            if (! needValue(i, v)) return false;
            if (! parseMousePosition(v, args.mouse_x, args.mouse_y)) {
                err = "--mouse-position expects normalized X,Y values";
                return false;
            }
            args.fixed_mouse_position = true;
        } else if (a == "--cache-path") {
            if (! needValue(i, args.cache_path)) return false;
        } else if (a == "--fps") {
            std::string v;
            if (! needValue(i, v)) return false;
            args.fps = std::atoi(v.c_str());
            if (args.fps <= 0) args.fps = 15;
        } else if (a == "--msaa") {
            std::string v;
            if (! needValue(i, v)) return false;
            if (! parsePositiveUint32(v, args.msaa_samples)) {
                err = "--msaa expects a positive integer";
                return false;
            }
        } else if (a == "--resolution") {
            std::string v;
            if (! needValue(i, v)) return false;
            auto x = v.find('x');
            if (x == std::string::npos) {
                err = "--resolution expects WxH";
                return false;
            }
            args.width  = std::atoi(v.substr(0, x).c_str());
            args.height = std::atoi(v.substr(x + 1).c_str());
        } else if (a.empty() || a[0] == '-') {
            err = "unknown option: " + a;
            return false;
        } else if (args.assets_uri.empty()) {
            args.assets_uri = a;
        } else if (args.uri.empty()) {
            args.uri = a;
        } else {
            err = "unexpected positional: " + a;
            return false;
        }
    }
    if (args.assets_uri.empty() || args.uri.empty()) {
        return false;
    }
    return true;
}

inline void printHelp(const char* prog) {
    std::fprintf(stderr,
                 "Usage: %s [options] <assets> <project.json>\n"
                 "       %s [options] <assets> <workshop-dir>\n"
                 "  --cache-path PATH       cache directory\n"
                 "  --fps N                 scene fps (default 15)\n"
                 "  --resolution WxH        output size (default 1280x720)\n"
                 "  --msaa N                final-output sample count (default 1)\n"
                 "  --user-properties FILE  initial scene user-properties JSON object\n"
                 "  --graphviz FILE          write the scene render graph to FILE\n"
                 "  --valid-layer            enable Vulkan validation layers\n"
                 "  --mouse-position X,Y     lock normalized pointer position (0..1)\n"
                 "  --diagnostics            print structured diagnostics on exit\n"
                 "  --shm                    force SHM output instead of DMA-BUF\n"
                 "  -h, --help              show this help\n",
                 prog,
                 prog);
}
