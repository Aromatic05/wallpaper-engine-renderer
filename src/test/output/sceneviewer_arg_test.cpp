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
    return 0;
}
