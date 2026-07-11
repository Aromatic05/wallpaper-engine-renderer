#include "backend/scene/internal/resources/WPPkgIndex.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using json = nlohmann::json;

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr, "corpus manifest failure: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

bool IsWorkshopId(const std::string& value) {
    return ! value.empty()
        && std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return std::isdigit(c) != 0;
           });
}

void ValidateGroup(const json& root,
                   std::string_view group,
                   const std::vector<std::string>& expectedKeys) {
    const auto it = root.find(group);
    Require(it != root.end() && it->is_object(), "version group must be an object");

    std::set<std::string> actualKeys;
    for (const auto& [key, value] : it->items()) {
        actualKeys.insert(key);
        Require(value.is_array() && ! value.empty(), "version sample list must be non-empty");
        std::set<std::string> ids;
        for (const auto& item : value) {
            Require(item.is_string(), "workshop id must be a string");
            const auto id = item.get<std::string>();
            Require(IsWorkshopId(id), "workshop id must contain only decimal digits");
            Require(ids.insert(id).second, "duplicate workshop id in version group");
        }
    }

    const std::set<std::string> expected(expectedKeys.begin(), expectedKeys.end());
    Require(actualKeys == expected, "version group does not provide the required coverage");
}

fs::path ResolveScenePackage(const fs::path& workshopDir) {
    std::ifstream projectFile(workshopDir / "project.json");
    if (! projectFile) return {};

    const auto project = json::parse(projectFile, nullptr, false, true);
    if (! project.is_object() || project.value("type", std::string {}) != "scene") return {};

    std::string source = project.value("file", std::string { "scene.json" });
    if (source.empty()) source = "scene.json";
    fs::path package = workshopDir / fs::path(source);
    if (package.extension() != ".pkg") package.replace_extension(".pkg");
    return package;
}

void ProbeAvailableWorkshops(const json& manifest) {
    const char* rootValue = std::getenv("WE_WORKSHOP_ROOT");
    if (! rootValue || ! *rootValue) return;

    const fs::path root(rootValue);
    Require(fs::is_directory(root), "WE_WORKSHOP_ROOT is not a directory");

    std::size_t probed = 0;
    for (const auto& [version, ids] : manifest.at("pkg").items()) {
        for (const auto& item : ids) {
            const auto workshopDir = root / item.get<std::string>();
            if (! fs::is_directory(workshopDir)) continue;

            const auto package = ResolveScenePackage(workshopDir);
            Require(! package.empty(), "manifest workshop is not a scene project");
            Require(fs::is_regular_file(package), "resolved scene package does not exist");

            const auto index = wallpaper::fs::ReadWPPkgIndex(package.string());
            Require(index.ok(), index.error().message);
            Require(index.value().version == "PKGV" + version,
                    "real workshop package version differs from manifest");
            ++probed;
        }
    }

    std::fprintf(stderr, "corpus manifest: probed %zu available workshop packages\n", probed);
}
} // namespace

int main() {
#ifndef WE_CORPUS_MANIFEST_PATH
#error WE_CORPUS_MANIFEST_PATH must be defined
#endif
    std::ifstream input(WE_CORPUS_MANIFEST_PATH);
    Require(static_cast<bool>(input), "cannot open corpus manifest");

    const auto manifest = json::parse(input, nullptr, false, true);
    Require(manifest.is_object(), "manifest must be a JSON object");
    Require(manifest.value("version", 0) == 1, "unsupported corpus manifest version");

    std::vector<std::string> pkgVersions;
    for (int version = 1; version <= 23; ++version) {
        char value[5] {};
        std::snprintf(value, sizeof(value), "%04d", version);
        pkgVersions.emplace_back(value);
    }
    ValidateGroup(manifest, "pkg", pkgVersions);
    ValidateGroup(manifest, "texb", { "1", "2", "3", "4" });
    ValidateGroup(manifest, "mdlv", { "4", "13", "14", "16", "17", "21" });
    ValidateGroup(manifest, "sceneFormat", { "0", "4", "6" });

    ProbeAvailableWorkshops(manifest);
    return 0;
}
