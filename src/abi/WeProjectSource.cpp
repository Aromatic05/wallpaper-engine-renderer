#include "WeProjectSource.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace wallpaper
{
namespace
{
std::string LowerAscii(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string Trim(std::string value) {
    const auto notSpace = [](unsigned char ch) { return ! std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool IsSafeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& component : path) {
        if (component == "..") return false;
    }
    return true;
}

Result<ProjectSourceInfo> Invalid(std::string message) {
    return Result<ProjectSourceInfo>::failure(ResultCode::InvalidArgument, std::move(message));
}

Result<ProjectSourceInfo> Missing(std::string message) {
    return Result<ProjectSourceInfo>::failure(ResultCode::NotFound, std::move(message));
}
} // namespace

Result<ProjectSourceInfo> ResolveProjectSource(std::string_view uri) {
    if (uri.empty()) return Invalid("source uri is empty");

    ProjectSourceInfo info;
    info.sourcePath = std::filesystem::path(uri);

    std::error_code error;
    if (std::filesystem::is_directory(info.sourcePath, error) && ! error) {
        info.projectJson = info.sourcePath / "project.json";
    } else {
        info.projectJson = info.sourcePath;
    }

    std::ifstream input(info.projectJson);
    if (! input) return Missing("cannot open project.json: " + info.projectJson.string());

    auto project = nlohmann::json::parse(input, nullptr, false, true);
    if (project.is_discarded()) {
        return Invalid("invalid JSON: " + info.projectJson.string());
    }

    const auto typeIt = project.find("type");
    if (typeIt == project.end() || ! typeIt->is_string()) {
        return Invalid("project.json is missing a string type field: "
                       + info.projectJson.string());
    }

    const std::string type = LowerAscii(typeIt->get<std::string>());
    const auto projectDir = info.projectJson.parent_path();
    if (type == "web") {
        info.type = BackendType::Web;
        info.backendUri = projectDir.string();
    } else if (type == "scene") {
        info.type = BackendType::WEScene;
        info.backendUri = (projectDir / "scene.pkg").string();

        const auto fileIt = project.find("file");
        if (fileIt != project.end()) {
            if (! fileIt->is_string()) {
                return Invalid("scene project.json file field must be a string: "
                               + info.projectJson.string());
            }
            const auto fileValue = Trim(fileIt->get<std::string>());
            if (! fileValue.empty()) {
                std::filesystem::path sourceFile(fileValue);
                if (! IsSafeRelativePath(sourceFile)) {
                    return Invalid("scene project.json file field must stay inside the project: "
                                   + info.projectJson.string());
                }
                sourceFile = projectDir / sourceFile;
                if (sourceFile.extension() != ".pkg") sourceFile.replace_extension("pkg");
                info.backendUri = sourceFile.lexically_normal().string();
            }
        }
    } else if (type == "video") {
        info.type = BackendType::Video;
        const auto fileIt = project.find("file");
        if (fileIt == project.end() || ! fileIt->is_string()) {
            return Invalid("video project.json is missing a string file field: "
                           + info.projectJson.string());
        }
        const auto fileValue = Trim(fileIt->get<std::string>());
        if (fileValue.empty()) {
            return Invalid("video project.json file field is empty: "
                           + info.projectJson.string());
        }
        const std::filesystem::path sourceFile(fileValue);
        if (! IsSafeRelativePath(sourceFile)) {
            return Invalid("video project.json file field must stay inside the project: "
                           + info.projectJson.string());
        }
        info.backendUri = (projectDir / sourceFile).lexically_normal().string();
    } else {
        return Result<ProjectSourceInfo>::failure(ResultCode::NotSupported,
                                                  "unsupported project type: " + type);
    }

    return Result<ProjectSourceInfo>::success(std::move(info));
}
} // namespace wallpaper
