#include "parser_probe.hpp"

#include "backend/scene/internal/parser/mdl/Format.hpp"

#include <nlohmann/json.hpp>

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

namespace
{
using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;
using json = nlohmann::json;

template<typename T>
void AppendPod(Bytes& bytes, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(value));
}

void AppendSizedString(Bytes& bytes, std::string_view value) {
    AppendPod<std::int32_t>(bytes, static_cast<std::int32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void AppendStamp(Bytes& bytes, std::string_view prefix, int version) {
    char stamp[9] {};
    std::snprintf(stamp, sizeof(stamp), "%.*s%04d",
                  static_cast<int>(prefix.size()), prefix.data(), version);
    bytes.insert(bytes.end(), stamp, stamp + sizeof(stamp));
}

Bytes BuildTexture() {
    Bytes bytes;
    AppendStamp(bytes, "TEXV", 5);
    AppendStamp(bytes, "TEXI", 1);
    AppendPod<std::int32_t>(bytes, 0);
    AppendPod<std::uint32_t>(bytes, 1u << 2);
    AppendPod<std::int32_t>(bytes, 16);
    AppendPod<std::int32_t>(bytes, 8);
    AppendPod<std::int32_t>(bytes, 16);
    AppendPod<std::int32_t>(bytes, 8);
    AppendPod<std::int32_t>(bytes, 0);
    AppendStamp(bytes, "TEXB", 4);
    AppendPod<std::int32_t>(bytes, 1);
    AppendPod<std::int32_t>(bytes, -1);
    AppendPod<std::int32_t>(bytes, 1234);
    AppendPod<std::int32_t>(bytes, 1);
    AppendPod<std::int32_t>(bytes, 16);
    AppendPod<std::int32_t>(bytes, 8);
    AppendPod<std::int32_t>(bytes, 0);
    AppendPod<std::int32_t>(bytes, 16 * 8 * 4);
    AppendPod<std::int32_t>(bytes, 16);
    const std::uint8_t payload[16] {
        0, 0, 0, 16, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 0, 0
    };
    bytes.insert(bytes.end(), std::begin(payload), std::end(payload));
    AppendStamp(bytes, "TEXS", 3);
    AppendPod<std::int32_t>(bytes, 1);
    AppendPod<std::int32_t>(bytes, 16);
    AppendPod<std::int32_t>(bytes, 8);
    AppendPod<std::int32_t>(bytes, 0);
    AppendPod<float>(bytes, 0.1f);
    AppendPod<float>(bytes, 0.0f);
    AppendPod<float>(bytes, 0.0f);
    AppendPod<float>(bytes, 16.0f);
    AppendPod<float>(bytes, 0.0f);
    AppendPod<float>(bytes, 0.0f);
    AppendPod<float>(bytes, 8.0f);
    return bytes;
}

void AppendMdlSection(Bytes& bytes, std::string_view type, int version, const Bytes& payload) {
    AppendStamp(bytes, type, version);
    const auto endOffset = static_cast<std::uint32_t>(bytes.size() + sizeof(std::uint32_t)
                                                     + payload.size());
    AppendPod<std::uint32_t>(bytes, endOffset);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}

Bytes BuildModel() {
    Bytes bytes;
    AppendStamp(bytes, "MDLV", 21);
    AppendPod<std::uint32_t>(bytes,
                             wallpaper::WPMDL_FLAG_POSITION | wallpaper::WPMDL_FLAG_UV);
    AppendPod<std::uint32_t>(bytes, 1);
    AppendPod<std::uint32_t>(bytes, 1);
    bytes.push_back(0xaa);
    bytes.push_back(0xbb);
    AppendMdlSection(bytes, "MDLS", 3, Bytes { 1, 2 });
    AppendMdlSection(bytes, "MDZZ", 1, Bytes { 3 });
    return bytes;
}

Bytes BuildPackage(const std::vector<std::pair<std::string, Bytes>>& entries) {
    Bytes bytes;
    AppendSizedString(bytes, "PKGV0023");
    AppendPod<std::int32_t>(bytes, static_cast<std::int32_t>(entries.size()));
    std::int32_t bodyOffset = 0;
    for (const auto& [path, content] : entries) {
        AppendSizedString(bytes, path);
        AppendPod<std::int32_t>(bytes, bodyOffset);
        AppendPod<std::int32_t>(bytes, static_cast<std::int32_t>(content.size()));
        bodyOffset += static_cast<std::int32_t>(content.size());
    }
    for (const auto& [path, content] : entries) {
        (void)path;
        bytes.insert(bytes.end(), content.begin(), content.end());
    }
    return bytes;
}

struct Fixture {
    fs::path root;

    Fixture() {
        root = fs::temp_directory_path()
            / ("we-parser-probe-test-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root);

        std::ofstream project(root / "project.json");
        project << R"({"type":"scene","file":"scene.json","title":"Probe"})";
        project.close();

        const std::string scene = R"({
            "objects":[
                {"id":7,"kind":"text","name":"Title"},
                {"id":2,"kind":"image","name":"Background"}
            ],
            "general":{"clearcolor":[0,0,0]},
            "camera":{"center":[0,0,0]}
        })";
        const std::vector<std::pair<std::string, Bytes>> entries {
            { "unknown.bin", Bytes { 9, 8, 7 } },
            { "models/test.mdl", BuildModel() },
            { "scene.json", Bytes(scene.begin(), scene.end()) },
            { "materials/good.tex", BuildTexture() },
            { "materials/bad.tex", Bytes { 'b', 'a', 'd' } },
        };
        const auto package = BuildPackage(entries);
        std::ofstream output(root / "scene.pkg", std::ios::binary);
        output.write(reinterpret_cast<const char*>(package.data()),
                     static_cast<std::streamsize>(package.size()));
    }

    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};
} // namespace

int main() {
#ifndef WE_PARSER_PROBE_SNAPSHOT_PATH
#error WE_PARSER_PROBE_SNAPSHOT_PATH must be defined
#endif
    Fixture fixture;
    auto actualResult = wallpaper::test::ProbeWorkshopProject(fixture.root);
    assert(actualResult);

    std::ifstream expectedInput(WE_PARSER_PROBE_SNAPSHOT_PATH);
    assert(expectedInput);
    const auto expected = json::parse(expectedInput, nullptr, false, true);
    assert(! expected.is_discarded());

    const auto patch = json::diff(expected, actualResult.value());
    if (! patch.empty()) {
        std::fprintf(stderr, "parser probe snapshot patch:\n%s\n", patch.dump(2).c_str());
        return 1;
    }

    auto changed = actualResult.value();
    changed["package"]["version"] = "PKGV9999";
    const auto proofPatch = json::diff(actualResult.value(), changed);
    assert(proofPatch.size() == 1);
    assert(proofPatch[0].at("op") == "replace");
    assert(proofPatch[0].at("path") == "/package/version");
    assert(proofPatch[0].at("value") == "PKGV9999");
    return 0;
}
