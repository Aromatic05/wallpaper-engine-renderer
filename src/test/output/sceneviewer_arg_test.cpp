#include "../../../standalone_view/arg.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace
{
[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "sceneviewer-arg-test: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

bool Parse(std::vector<std::string> values, Args& args, std::string& error) {
    std::vector<char*> argv;
    argv.reserve(values.size());
    for (auto& value : values) argv.push_back(value.data());
    return parseArgs(static_cast<int>(argv.size()), argv.data(), args, error);
}
} // namespace

int main() {
    {
        Args args;
        std::string error;
        Require(Parse({ "sceneviewer", "/assets", "/wallpaper" }, args, error),
                "default arguments were rejected");
        Require(args.msaa_samples == 1, "default MSAA request must remain 1x");
    }
    {
        Args args;
        std::string error;
        Require(Parse({ "sceneviewer", "--msaa", "4", "/assets", "/wallpaper" },
                      args,
                      error),
                "valid MSAA request was rejected");
        Require(args.msaa_samples == 4, "MSAA argument was not preserved");
    }
    {
        Args args;
        std::string error;
        Require(! Parse({ "sceneviewer", "--msaa", "0", "/assets", "/wallpaper" },
                        args,
                        error),
                "zero MSAA request must be rejected by the CLI");
        Require(error.find("--msaa") != std::string::npos,
                "zero MSAA error did not identify the option");
    }
    {
        Args args;
        std::string error;
        Require(! Parse({ "sceneviewer", "--msaa", "many", "/assets", "/wallpaper" },
                        args,
                        error),
                "non-numeric MSAA request must be rejected");
        Require(error.find("--msaa") != std::string::npos,
                "invalid MSAA error did not identify the option");
    }
    {
        Args args;
        std::string error;
        Require(Parse({ "sceneviewer",
                        "--user-properties",
                        "/tmp/user.json",
                        "--graphviz",
                        "/tmp/scene \"graph\".dot",
                        "--valid-layer",
                        "--mouse-position",
                        "0.25,0.75",
                        "--diagnostics",
                        "/assets",
                        "/wallpaper" },
                      args,
                      error),
                "complete ABI option set was rejected");
        Require(args.user_properties_path == "/tmp/user.json",
                "user-properties path was not preserved");
        Require(args.graphviz_path == "/tmp/scene \"graph\".dot",
                "graphviz path was not preserved");
        Require(args.enable_valid_layer, "validation-layer flag was not preserved");
        Require(args.fixed_mouse_position && args.mouse_x == 0.25f && args.mouse_y == 0.75f,
                "fixed mouse position was not preserved");
        Require(args.print_diagnostics, "diagnostics flag was not preserved");

        const std::string options = buildSourceOptionsJson(args, R"({"enabled":true})");
        Require(options ==
                    R"({"version":1,"scene":{"userProperties":{"enabled":true},"graphviz":{"enabled":true,"path":"/tmp/scene \"graph\".dot"}}})",
                "source options JSON did not preserve properties and escape graphviz path");
    }
    {
        Args args;
        std::string error;
        Require(! Parse({ "sceneviewer",
                          "--mouse-position",
                          "1.25,0.5",
                          "/assets",
                          "/wallpaper" },
                        args,
                        error),
                "out-of-range mouse position must be rejected");
        Require(error.find("--mouse-position") != std::string::npos,
                "mouse-position error did not identify the option");
    }
    {
        Args args;
        std::string error;
        Require(! Parse({ "sceneviewer",
                          "--mouse-position",
                          "0.5",
                          "/assets",
                          "/wallpaper" },
                        args,
                        error),
                "incomplete mouse position must be rejected");
    }
    return 0;
}
