#include "parser_probe.hpp"

#include "abi/WeProjectSource.hpp"
#include "backend/scene/internal/parser/WPTexHeaderParser.hpp"
#include "backend/scene/internal/parser/mdl/Format.hpp"
#include "backend/scene/internal/parser/mdl/Section.hpp"
#include "backend/scene/internal/resources/WPPkgIndex.hpp"
#include "fs/CBinaryStream.h"
#include "fs/LimitedBinaryStream.h"
#include "scene/Image.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace wallpaper::test
{
namespace
{
using json = nlohmann::json;
namespace fs = std::filesystem;

bool IsKnownMdlSection(std::string_view type) {
    static constexpr std::array<std::string_view, 5> known {
        "MDLS", "MDAT", "MDLA", "MDMP", "MDLE"
    };
    return std::find(known.begin(), known.end(), type) != known.end();
}

std::shared_ptr<wallpaper::fs::IBinaryStream> OpenEntry(
    const fs::path& packagePath,
    const wallpaper::fs::WPPkgIndexEntry& entry) {
    auto package = wallpaper::fs::CreateCBinaryStream(packagePath.string());
    if (! package) return nullptr;
    return std::make_shared<wallpaper::fs::LimitedBinaryStream>(
        std::move(package), entry.offset, entry.length);
}

std::string RelativeSourceName(const fs::path& projectRoot, const fs::path& sourcePath) {
    std::error_code error;
    auto relative = fs::relative(sourcePath, projectRoot, error);
    if (error || relative.empty()) return sourcePath.filename().generic_string();
    return relative.generic_string();
}

std::string SceneEntryPath(const fs::path& projectRoot, const fs::path& packagePath) {
    fs::path scenePath = fs::path(RelativeSourceName(projectRoot, packagePath));
    scenePath.replace_extension(".json");
    auto value = scenePath.lexically_normal().generic_string();
    if (! value.starts_with('/')) value.insert(value.begin(), '/');
    return value;
}

json ProbeScene(wallpaper::fs::IBinaryStream& stream,
                std::string_view path,
                std::vector<std::string>& warnings) {
    json summary {
        { "path", path },
        { "parsed", false },
        { "rootKeys", json::array() },
        { "objectCount", 0 },
        { "objectKinds", json::object() },
    };

    const auto source = stream.ReadAllStr();
    auto scene = json::parse(source, nullptr, false, true);
    if (! scene.is_object()) {
        warnings.push_back(std::string(path) + ": invalid scene JSON");
        return summary;
    }

    for (const auto& [key, value] : scene.items()) {
        (void)value;
        summary["rootKeys"].push_back(key);
    }

    const auto objects = scene.find("objects");
    if (objects != scene.end() && objects->is_array()) {
        summary["objectCount"] = objects->size();
        std::map<std::string, std::size_t> kinds;
        for (const auto& object : *objects) {
            if (! object.is_object()) {
                ++kinds["<invalid>"];
                continue;
            }
            const auto kind = object.find("kind");
            if (kind != object.end() && kind->is_string()) {
                ++kinds[kind->get<std::string>()];
            } else {
                ++kinds["<unknown>"];
            }
        }
        for (const auto& [kind, count] : kinds) summary["objectKinds"][kind] = count;
    }
    summary["parsed"] = true;
    return summary;
}

json ProbeTexture(wallpaper::fs::IBinaryStream& stream,
                  std::string_view path,
                  std::vector<std::string>& warnings) {
    json summary {
        { "path", path },
        { "ok", false },
    };
    auto parsed = ParseWPTexHeader(stream);
    if (! parsed) {
        summary["errorCode"] = static_cast<int>(parsed.error().code);
        summary["error"] = parsed.error().message;
        warnings.push_back(std::string(path) + ": " + parsed.error().message);
        return summary;
    }

    const auto& header = parsed.value();
    summary["ok"] = true;
    summary["texv"] = header.extraHeader.at("texv").val;
    summary["texi"] = header.extraHeader.at("texi").val;
    summary["texb"] = header.extraHeader.at("texb").val;
    if (const auto texs = header.extraHeader.find("texs"); texs != header.extraHeader.end()) {
        summary["texs"] = texs->second.val;
    }
    summary["format"] = ToString(header.format);
    summary["width"] = header.width;
    summary["height"] = header.height;
    summary["mapWidth"] = header.mapWidth;
    summary["mapHeight"] = header.mapHeight;
    summary["imageCount"] = header.count;
    summary["sprite"] = header.isSprite;
    summary["spriteFrames"] = header.spriteAnim.Frames().size();
    summary["videoPayload"] = header.isVideoTexture;
    return summary;
}

json ProbeModel(wallpaper::fs::IBinaryStream& stream,
                std::string_view path,
                std::vector<json>& unknownSections,
                std::vector<std::string>& warnings) {
    json summary {
        { "path", path },
        { "ok", false },
        { "sections", json::array() },
    };

    auto header = ParseWPMdlHeader(stream);
    if (! header) {
        summary["errorCode"] = static_cast<int>(header.error().code);
        summary["error"] = header.error().message;
        warnings.push_back(std::string(path) + ": " + header.error().message);
        return summary;
    }

    summary["ok"] = true;
    summary["mdlv"] = header.value().mdlv;
    summary["flags"] = header.value().mdl_flag;
    summary["meshCount"] = header.value().mesh_count;
    summary["vertexStride"] = WPMdlVertexStride(header.value().mdl_flag);

    while (stream.Tell() >= 0 && stream.Tell() < stream.Size()) {
        const auto remaining = stream.Size() - stream.Tell();
        auto section = FindNextWPMdlSection(stream, {}, remaining);
        if (! section) break;

        json sectionSummary {
            { "type", section.value().Type() },
            { "version", section.value().version },
            { "payloadBytes", section.value().end_offset - section.value().payload_offset },
        };
        summary["sections"].push_back(sectionSummary);
        if (! IsKnownMdlSection(section.value().Type())) {
            json unknown = sectionSummary;
            unknown["model"] = path;
            unknownSections.push_back(std::move(unknown));
        }
        if (! stream.SeekSet(section.value().end_offset)) break;
    }
    return summary;
}
} // namespace

Result<nlohmann::json> ProbeWorkshopProject(const std::filesystem::path& projectPath) {
    auto source = ResolveProjectSource(projectPath.string());
    if (! source) return Result<json>(source.error());
    if (source.value().type != BackendType::WEScene) {
        return Result<json>::failure(ResultCode::NotSupported,
                                     "parser probe supports scene projects only");
    }

    const fs::path projectRoot = source.value().projectJson.parent_path();
    const fs::path packagePath = source.value().backendUri;
    auto package = wallpaper::fs::ReadWPPkgIndex(packagePath.string());
    if (! package) return Result<json>(package.error());

    std::vector<wallpaper::fs::WPPkgIndexEntry> entries = package.value().entries;
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.path < right.path;
    });

    json output {
        { "schemaVersion", 1 },
        { "project", {
            { "type", "scene" },
            { "source", RelativeSourceName(projectRoot, packagePath) },
        } },
        { "package", {
            { "version", package.value().version },
            { "fileCount", entries.size() },
            { "entries", json::array() },
        } },
        { "scene", nullptr },
        { "textures", json::array() },
        { "models", json::array() },
        { "unknownSections", json::array() },
        { "warnings", json::array() },
    };

    for (const auto& entry : entries) {
        output["package"]["entries"].push_back({
            { "path", entry.path },
            { "size", entry.length },
        });
    }

    std::vector<std::string> warnings;
    std::vector<json> unknownSections;
    const auto scenePath = SceneEntryPath(projectRoot, packagePath);
    for (const auto& entry : entries) {
        auto stream = OpenEntry(packagePath, entry);
        if (! stream) {
            warnings.push_back(entry.path + ": failed to open package entry");
            continue;
        }
        if (entry.path == scenePath) {
            output["scene"] = ProbeScene(*stream, entry.path, warnings);
        } else if (entry.path.ends_with(".tex")) {
            output["textures"].push_back(ProbeTexture(*stream, entry.path, warnings));
        } else if (entry.path.ends_with(".mdl")) {
            output["models"].push_back(ProbeModel(*stream,
                                                   entry.path,
                                                   unknownSections,
                                                   warnings));
        }
    }

    if (output["scene"].is_null()) {
        warnings.push_back(scenePath + ": scene JSON entry is missing");
        output["scene"] = {
            { "path", scenePath },
            { "parsed", false },
            { "rootKeys", json::array() },
            { "objectCount", 0 },
            { "objectKinds", json::object() },
        };
    }

    std::sort(unknownSections.begin(), unknownSections.end(), [](const json& left, const json& right) {
        return std::tie(left.at("model"), left.at("type"), left.at("version"))
               < std::tie(right.at("model"), right.at("type"), right.at("version"));
    });
    for (auto& section : unknownSections) output["unknownSections"].push_back(std::move(section));

    std::sort(warnings.begin(), warnings.end());
    for (auto& warning : warnings) output["warnings"].push_back(std::move(warning));
    return Result<json>::success(std::move(output));
}
} // namespace wallpaper::test
