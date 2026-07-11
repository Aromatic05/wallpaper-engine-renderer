#include "backend/scene/internal/parser/WPMdlParser.hpp"
#include "backend/scene/internal/scene/include/scene/SceneMesh.h"
#include "fs/Fs.h"
#include "fs/MemBinaryStream.h"
#include "fs/VFS.h"

#include <array>
#include <cmath>
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

void AppendIdentityMatrix(Bytes& bytes, float translationX = 0.0f) {
    std::array<float, 16> matrix {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        translationX, 0.0f, 0.0f, 1.0f,
    };
    AppendPod(bytes, matrix);
}

std::size_t BeginSection(Bytes& bytes, std::string_view type, int version) {
    Require(type.size() == 4, "section type must contain four bytes");
    char stamp[9] {};
    std::snprintf(stamp, sizeof(stamp), "%.*s%04d",
                  static_cast<int>(type.size()), type.data(), version);
    bytes.insert(bytes.end(), stamp, stamp + sizeof(stamp));
    const std::size_t endOffsetPosition = bytes.size();
    AppendPod<std::uint32_t>(bytes, 0);
    return endOffsetPosition;
}

void EndSection(Bytes& bytes, std::size_t endOffsetPosition) {
    const auto endOffset = static_cast<std::uint32_t>(bytes.size());
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&endOffset);
    for (std::size_t index = 0; index < sizeof(endOffset); ++index) {
        bytes[endOffsetPosition + index] = raw[index];
    }
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

void AppendGenericPuppetMesh(Bytes& bytes) {
    constexpr std::uint32_t flags = wallpaper::WPMDL_FLAG_POSITION
        | wallpaper::WPMDL_FLAG_UV | wallpaper::WPMDL_FLAG_SKIN_BLEND
        | wallpaper::WPMDL_FLAG_SKIN_WEIGHT;
    AppendCString(bytes, "materials/puppet.json");
    AppendPod<std::uint32_t>(bytes, 0);
    AppendPod<std::uint32_t>(bytes, 3 * wallpaper::WPMdlVertexStride(flags));
    for (std::uint32_t index = 0; index < 3; ++index) {
        AppendPod(bytes, std::array<float, 3> {
            static_cast<float>(index), static_cast<float>(index), 0.0f
        });
        AppendPod(bytes, std::array<std::uint32_t, 4> { 0, 0, 0, 0 });
        AppendPod(bytes, std::array<float, 4> { 1.0f, 0.0f, 0.0f, 0.0f });
        AppendPod(bytes, std::array<float, 2> {
            static_cast<float>(index) * 0.5f, 0.5f
        });
    }
    AppendPod<std::uint32_t>(bytes, 3 * sizeof(std::uint16_t));
    AppendPod<std::uint16_t>(bytes, 0);
    AppendPod<std::uint16_t>(bytes, 1);
    AppendPod<std::uint16_t>(bytes, 2);
}

Bytes BuildSectionedPuppet(bool invalidWorldBind = false,
                           bool malformedAnimation = false) {
    Bytes bytes;
    constexpr std::uint32_t flags = wallpaper::WPMDL_FLAG_POSITION
        | wallpaper::WPMDL_FLAG_UV | wallpaper::WPMDL_FLAG_SKIN_BLEND
        | wallpaper::WPMDL_FLAG_SKIN_WEIGHT;
    AppendHeader(bytes, 13, flags, 1);
    AppendGenericPuppetMesh(bytes);

    bytes.insert(bytes.end(), { 0, 0x7f, 0 });
    const auto skeletonEnd = BeginSection(bytes, "MDLS", 1);
    AppendPod<std::uint16_t>(bytes, 1);
    AppendPod<std::uint16_t>(bytes, 0);
    AppendCString(bytes, "root");
    AppendPod<std::int32_t>(bytes, 0);
    AppendPod<std::uint32_t>(bytes, 0xffffffffu);
    AppendPod<std::uint32_t>(bytes, 64);
    AppendIdentityMatrix(bytes);
    AppendCString(bytes, "");
    EndSection(bytes, skeletonEnd);

    const auto attachmentEnd = BeginSection(bytes, "MDAT", 1);
    AppendPod<std::uint16_t>(bytes, 1);
    AppendPod<std::uint16_t>(bytes, 0);
    AppendCString(bytes, "socket");
    AppendIdentityMatrix(bytes, 2.0f);
    EndSection(bytes, attachmentEnd);

    const auto unknownEnd = BeginSection(bytes, "MDZZ", 1);
    AppendPod<std::uint32_t>(bytes, 0xdeadbeefu);
    EndSection(bytes, unknownEnd);

    const auto animationEnd = BeginSection(bytes, "MDLA", 1);
    AppendPod<std::uint32_t>(bytes, malformedAnimation ? 1u : 0u);
    EndSection(bytes, animationEnd);

    const auto morphEnd = BeginSection(bytes, "MDMP", 1);
    AppendPod<std::uint16_t>(bytes, 1);
    AppendPod<float>(bytes, 0.5f);
    AppendPod<std::uint16_t>(bytes, 7);
    AppendPod<std::uint16_t>(bytes, 0);
    AppendPod<std::uint32_t>(bytes, 1);
    AppendPod<std::uint32_t>(bytes, 0);
    AppendCString(bytes, "shape");
    AppendPod<std::uint32_t>(bytes, 6);
    AppendPod<std::uint32_t>(bytes, 123);
    AppendPod<std::uint16_t>(bytes, 10);
    AppendPod<std::uint16_t>(bytes, 20);
    AppendPod<std::uint16_t>(bytes, 30);
    AppendPod<std::uint16_t>(bytes, 40);
    EndSection(bytes, morphEnd);

    const auto worldBindEnd = BeginSection(bytes, "MDLE", 1);
    AppendPod<std::uint32_t>(bytes, invalidWorldBind ? 60u : 64u);
    AppendIdentityMatrix(bytes, 3.0f);
    EndSection(bytes, worldBindEnd);
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

bool TryParseModel(std::string_view path, const Bytes& bytes, wallpaper::WPMdl& mdl) {
    wallpaper::fs::VFS vfs;
    vfs.Mount("/assets",
              std::make_unique<MemoryFs>(
                  std::unordered_map<std::string, std::string> {
                      { "/" + std::string(path), ToString(bytes) },
                  }),
              "mdl-test");
    return wallpaper::WPMdlParser::Parse(path, vfs, mdl);
}

wallpaper::WPMdl ParseModel(std::string_view path, const Bytes& bytes) {
    wallpaper::WPMdl mdl;
    Require(TryParseModel(path, bytes, mdl), "WPMdlParser::Parse failed");
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

void TestSectionDispatchIntegration() {
    auto mdl = ParseModel("models/sectioned.mdl", BuildSectionedPuppet());
    Require(mdl.kind == wallpaper::WPMdl::MeshKind::Puppet,
            "skinned generic mesh must remain a puppet");
    Require(mdl.mdls == 1 && mdl.mdla == 1 && mdl.mdmp == 1 && mdl.mdle == 1,
            "section versions were not preserved");
    Require(mdl.puppet != nullptr && mdl.puppet->bones.size() == 1,
            "MDLS bone payload mismatch");
    Require(mdl.puppet->bones[0].name == "root"
                && mdl.puppet->bones[0].noParent(),
            "MDLS bone identity mismatch");
    Require(mdl.puppet->attachments.size() == 1
                && mdl.puppet->attachments[0].name == "socket",
            "MDAT attachment payload mismatch");
    Require(std::abs(mdl.puppet->attachments[0].transform.translation().x() - 2.0f)
                < 0.0001f,
            "MDAT attachment transform mismatch");
    Require(mdl.puppet->anims.empty(), "zero-count MDLA should keep an empty animation list");

    Require(mdl.morph_sections.size() == 1,
            "MDMP event count mismatch");
    const auto& morphEvent = mdl.morph_sections.front();
    Require(std::abs(morphEvent.event_time - 0.5f) < 0.0001f
                && morphEvent.event_id == 7 && morphEvent.sections.size() == 1,
            "MDMP event metadata mismatch");
    const auto& morphData = morphEvent.sections.front();
    Require(morphData.shape_id == 1 && morphData.tag == "shape" && morphData.hash == 123,
            "MDMP section metadata mismatch");
    Require(morphData.vertices
                == std::vector<std::array<std::uint16_t, 3>> { { 10, 20, 30 } }
                && morphData.vertex_trailers == std::vector<std::uint16_t> { 40 },
            "MDMP vertex payload mismatch");

    const auto& bone = mdl.puppet->bones.front();
    Require(bone.has_file_world_bind,
            "MDLE world-bind matrix must be marked present");
    Require(std::abs(bone.file_world_bind.translation().x() - 3.0f) < 0.0001f,
            "MDLE world-bind translation mismatch");
}

void TestMalformedAnimationRecovery() {
    wallpaper::WPMdl mdl;
    Require(TryParseModel("models/malformed-animation.mdl",
                          BuildSectionedPuppet(false, true),
                          mdl),
            "malformed MDLA should recover at its declared end offset");
    Require(mdl.puppet != nullptr && mdl.puppet->anims.empty(),
            "malformed MDLA must leave a bind-pose-only puppet");
    Require(mdl.mdmp == 1 && mdl.mdle == 1
                && mdl.puppet->bones.front().has_file_world_bind,
            "sections after malformed MDLA must still be parsed");
}

void TestInvalidWorldBindRejected() {
    wallpaper::WPMdl mdl;
    Require(! TryParseModel("models/invalid-world-bind.mdl",
                            BuildSectionedPuppet(true, false),
                            mdl),
            "MDLE payload size mismatch must reject the model");
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
    TestSectionDispatchIntegration();
    TestMalformedAnimationRecovery();
    TestInvalidWorldBindRejected();
    return 0;
}
