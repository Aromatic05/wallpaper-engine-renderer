#include "backend/scene/internal/parser/WPMdlParser.hpp"
#include "backend/scene/internal/scene/include/scene/SceneMesh.h"
#include "fs/Fs.h"
#include "fs/MemBinaryStream.h"
#include "fs/VFS.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace
{
using Bytes = std::vector<std::uint8_t>;

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr, "mdl parser integration failure: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

template<typename T>
void AppendPod(Bytes& bytes, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

void AppendCString(Bytes& bytes, std::string_view value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(0);
}

void AppendHeader(Bytes& bytes, int version, std::uint32_t flags, std::uint32_t meshCount) {
    char stamp[9] {};
    std::snprintf(stamp, sizeof(stamp), "MDLV%04d", version);
    bytes.insert(bytes.end(), stamp, stamp + sizeof(stamp));
    AppendPod(bytes, flags);
    AppendPod<std::uint32_t>(bytes, 1);
    AppendPod(bytes, meshCount);
}

void AppendGenericImageMesh(Bytes& bytes,
                            int version,
                            std::string_view material,
                            float xOffset) {
    const std::uint32_t flags = wallpaper::WPMDL_FLAG_POSITION | wallpaper::WPMDL_FLAG_UV;
    AppendCString(bytes, material);
    AppendPod<std::uint32_t>(bytes, 0);
    if (version >= 17) {
        AppendPod(bytes, std::array<float, 3> { xOffset, 0.0f, 0.0f });
        AppendPod(bytes, std::array<float, 3> { xOffset + 1.0f, 1.0f, 0.0f });
    }
    if (version > 14) AppendPod(bytes, flags);

    AppendPod<std::uint32_t>(bytes, 3 * wallpaper::WPMdlVertexStride(flags));
    for (std::uint32_t index = 0; index < 3; ++index) {
        AppendPod(bytes, std::array<float, 3> {
            xOffset + static_cast<float>(index), static_cast<float>(index), 0.0f
        });
        AppendPod(bytes, std::array<float, 2> {
            static_cast<float>(index) * 0.5f, 0.5f
        });
    }

    AppendPod<std::uint32_t>(bytes, 3 * sizeof(std::uint16_t));
    AppendPod<std::uint16_t>(bytes, 0);
    AppendPod<std::uint16_t>(bytes, 1);
    AppendPod<std::uint16_t>(bytes, 2);
}

Bytes BuildGenericMultiMesh() {
    Bytes bytes;
    const std::uint32_t flags = wallpaper::WPMDL_FLAG_POSITION | wallpaper::WPMDL_FLAG_UV;
    AppendHeader(bytes, 17, flags, 2);
    AppendGenericImageMesh(bytes, 17, "materials/first.json", 0.0f);
    AppendGenericImageMesh(bytes, 17, "materials/second.json", 10.0f);
    return bytes;
}

Bytes BuildLegacyStaticImageMesh() {
    Bytes bytes;
    AppendHeader(bytes, 13, 9, 1);
    AppendCString(bytes, "materials/legacy.json");
    AppendPod<std::uint32_t>(bytes, 0);
    AppendPod<std::uint32_t>(bytes, 0x0000000fu);
    AppendPod<std::uint32_t>(bytes, 3 * 48);
    for (std::uint32_t index = 0; index < 3; ++index) {
        AppendPod(bytes, std::array<float, 3> {
            static_cast<float>(index), static_cast<float>(index), 0.0f
        });
        AppendPod(bytes, std::array<float, 3> { 0.0f, 0.0f, 1.0f });
        AppendPod(bytes, std::array<float, 4> { 1.0f, 0.0f, 0.0f, 1.0f });
        AppendPod(bytes, std::array<float, 2> {
            static_cast<float>(index) * 0.5f, 0.5f
        });
    }
    AppendPod<std::uint32_t>(bytes, 3 * sizeof(std::uint16_t));
    AppendPod<std::uint16_t>(bytes, 0);
    AppendPod<std::uint16_t>(bytes, 1);
    AppendPod<std::uint16_t>(bytes, 2);
    return bytes;
}

class MemoryFs final : public wallpaper::fs::Fs {
public:
    explicit MemoryFs(std::unordered_map<std::string, std::string> files)
        : files_(std::move(files)) {}

    bool Contains(std::string_view path) const override {
        return files_.contains(std::string(path));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStream> Open(std::string_view path) override {
        const auto it = files_.find(std::string(path));
        if (it == files_.end()) return nullptr;
        return std::make_shared<wallpaper::fs::MemBinaryStream>(
            std::vector<std::uint8_t>(it->second.begin(), it->second.end()));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStreamW> OpenW(std::string_view) override {
        return nullptr;
    }

private:
    std::unordered_map<std::string, std::string> files_;
};

std::string ToString(const Bytes& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

wallpaper::WPMdl ParseModel(std::string_view path, Bytes bytes) {
    wallpaper::fs::VFS vfs;
    vfs.Mount("/assets",
              std::make_unique<MemoryFs>(
                  std::unordered_map<std::string, std::string> {
                      { "/" + std::string(path), ToString(bytes) },
                  }),
              "mdl-test");
    wallpaper::WPMdl mdl;
    Require(wallpaper::WPMdlParser::Parse(path, vfs, mdl), "WPMdlParser::Parse failed");
    return mdl;
}

void TestGenericMultiMeshIntegration() {
    auto mdl = ParseModel("models/multi.mdl", BuildGenericMultiMesh());
    Require(mdl.kind == wallpaper::WPMdl::MeshKind::StaticImage,
            "non-skinned generic meshes must become static image geometry");
    Require(mdl.meshes.size() == 2, "all generic meshes must be retained");
    Require(mdl.mat_json_file == "materials/first.json",
            "legacy material view must use first mesh material");
    Require(mdl.vertexs.size() == 6, "multi-mesh vertices must be merged");
    Require(mdl.indices.size() == 2, "multi-mesh triangles must be merged");
    Require(mdl.indices[0] == std::array<std::uint32_t, 3> { 0, 1, 2 },
            "first mesh indices mismatch");
    Require(mdl.indices[1] == std::array<std::uint32_t, 3> { 3, 4, 5 },
            "second mesh indices must be rebased");
    Require(mdl.vertexs[3].position[0] == 10.0f,
            "second mesh vertex payload mismatch");
}

void TestLegacyFallbackIntegration() {
    auto mdl = ParseModel("models/legacy.mdl", BuildLegacyStaticImageMesh());
    Require(mdl.kind == wallpaper::WPMdl::MeshKind::StaticImage,
            "legacy marker mesh must remain static image geometry");
    Require(mdl.meshes.empty(), "legacy fallback must not fabricate generic mesh records");
    Require(mdl.vertexs.size() == 3 && mdl.indices.size() == 1,
            "legacy marker mesh geometry mismatch");
    Require(mdl.mat_json_file == "materials/legacy.json",
            "legacy material path mismatch");
}

void TestRuntimeUint32IndexUpload() {
    wallpaper::WPMdl mdl;
    mdl.vertexs.resize(65'537);
    mdl.indices.push_back({ 0, 1, 65'536 });

    wallpaper::SceneMesh mesh;
    wallpaper::WPMdlParser::GenPuppetMesh(mesh, mdl);
    Require(mesh.VertexCount() == 1 && mesh.IndexCount() == 1,
            "runtime mesh arrays were not created");
    const auto& indexArray = mesh.GetIndexArray(0);
    Require(indexArray.DataCount() == 3, "runtime index count mismatch");
    Require(indexArray.Data()[2] == 65'536,
            "runtime uint32 index must not be truncated to uint16");
}
} // namespace

int main() {
    TestGenericMultiMeshIntegration();
    TestLegacyFallbackIntegration();
    TestRuntimeUint32IndexUpload();
    return 0;
}
