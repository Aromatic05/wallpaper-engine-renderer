#include "parser_probe.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        std::cerr << "usage: corpus-parser-probe <project-dir-or-json> [--compare expected.json]\n";
        return 1;
    }
    if (argc == 4 && std::string(argv[2]) != "--compare") {
        std::cerr << "unknown option: " << argv[2] << "\n";
        return 1;
    }

    auto result = wallpaper::test::ProbeWorkshopProject(std::filesystem::path(argv[1]));
    if (! result) {
        std::cerr << result.error().message << "\n";
        return 1;
    }

    if (argc == 2) {
        std::cout << result.value().dump(2) << '\n';
        return 0;
    }

    std::ifstream expectedInput(argv[3]);
    if (! expectedInput) {
        std::cerr << "cannot open expected snapshot: " << argv[3] << "\n";
        return 1;
    }
    auto expected = nlohmann::json::parse(expectedInput, nullptr, false, true);
    if (expected.is_discarded()) {
        std::cerr << "invalid expected snapshot JSON: " << argv[3] << "\n";
        return 1;
    }

    const auto patch = nlohmann::json::diff(expected, result.value());
    std::cout << patch.dump(2) << '\n';
    return patch.empty() ? 0 : 2;
}
