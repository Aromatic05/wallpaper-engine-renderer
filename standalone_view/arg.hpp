#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

constexpr std::string_view ARG_PROJECT_JSON = "<project.json>";

struct Args {
    std::string assets_uri;
    std::string uri;
    std::string cache_path;
    int32_t       fps    { 15 };
    int32_t       width  { 1280 };
    int32_t       height { 720 };
    std::uint32_t msaa_samples { 1 };
    bool          force_shm { false };
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
                 "  --cache-path PATH    cache directory\n"
                 "  --fps N              scene fps (default 15)\n"
                 "  --resolution WxH     output size (default 1280x720)\n"
                 "  --msaa N             final-output sample count (default 1)\n"
                 "  --shm                force SHM output instead of DMA-BUF\n"
                 "  -h, --help           show this help\n",
                 prog,
                 prog);
}
