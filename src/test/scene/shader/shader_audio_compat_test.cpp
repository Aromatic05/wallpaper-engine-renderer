#include "backend/scene/internal/parser/WPShaderParserTestHooks.hpp"
#include "backend/scene/internal/parser/WPShaderParser.hpp"
#include "fs/VFS.h"

#include <cstdio>
#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

#include <span>
#include <vector>
namespace
{
[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr, "shader audio compatibility test failure: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

void TestExactPackedIndex() {
    const std::string source = R"(
uniform float g_AudioSpectrum64Left[64];
float sample(float barID) {
    return g_AudioSpectrum64Left[barID / 4][barID % 4];
}
)";
    const auto normalized = wallpaper::test::NormalizePackedAudioSpectrumAccess(source);
    Require(normalized.find("g_AudioSpectrum64Left[(int)(barID)]") != std::string::npos,
            "packed bar index should collapse to one integer index");
    Require(normalized.find("[barID / 4][barID % 4]") == std::string::npos,
            "legacy packed access should be removed");
    Require(normalized.find("g_AudioSpectrum64Left[64]") != std::string::npos,
            "single-dimensional uniform declaration must remain unchanged");
}

void TestGenericPackedIndex() {
    const std::string source =
        "float v = g_AudioSpectrum32Right[group + 1][component];";
    const auto normalized = wallpaper::test::NormalizePackedAudioSpectrumAccess(source);
    Require(normalized.find(
                "g_AudioSpectrum32Right[((int)(group + 1) * 4 + (int)(component))]")
                != std::string::npos,
            "generic packed access should flatten group and component");
}

void TestOnlyAudioArraysAreRewritten() {
    const std::string source = R"(
// g_AudioSpectrum16Left[i / 4][i % 4]
/* g_AudioSpectrum32Left[j / 4][j % 4] */
const char* diagnostic = "g_AudioSpectrum64Right[k / 4][k % 4]";
float ordinary = values[i][j];
float audio = g_AudioSpectrum16Right[i / 4][i % 4];
)";
    const auto normalized = wallpaper::test::NormalizePackedAudioSpectrumAccess(source);
    Require(normalized.find("// g_AudioSpectrum16Left[i / 4][i % 4]") != std::string::npos,
            "line comments must remain unchanged");
    Require(normalized.find("/* g_AudioSpectrum32Left[j / 4][j % 4] */")
                != std::string::npos,
            "block comments must remain unchanged");
    Require(normalized.find("\"g_AudioSpectrum64Right[k / 4][k % 4]\"")
                != std::string::npos,
            "string literals must remain unchanged");
    Require(normalized.find("values[i][j]") != std::string::npos,
            "ordinary double-index arrays must remain unchanged");
    Require(normalized.find("g_AudioSpectrum16Right[(int)(i)]") != std::string::npos,
            "live audio access should be rewritten");
}

void TestMalformedAccessStaysVisible() {
    const std::string source = "float v = g_AudioSpectrum16Left[i][;";
    const auto normalized = wallpaper::test::NormalizePackedAudioSpectrumAccess(source);
    Require(normalized == source,
            "malformed access should remain available for downstream diagnostics");
}
void TestPackedAudioShaderCompiles() {
    wallpaper::fs::VFS vfs;
    wallpaper::WPShaderInfo shaderInfo;
    std::array<wallpaper::WPShaderUnit, 2> units {
        wallpaper::WPShaderUnit {
            .stage = wallpaper::ShaderType::VERTEX,
            .src = R"(
                attribute vec3 a_Position;
                varying vec2 v_TexCoord;
                void main() {
                    v_TexCoord = a_Position.xy;
                    gl_Position = vec4(a_Position, 1.0);
                }
            )",
            .preprocess_info = {},
        },
        wallpaper::WPShaderUnit {
            .stage = wallpaper::ShaderType::FRAGMENT,
            .src = R"(
                varying vec2 v_TexCoord;
                uniform float g_AudioSpectrum64Left[64];
                void main() {
                    float barID = floor(v_TexCoord.x * 64.0);
                    float value = g_AudioSpectrum64Left[barID / 4][barID % 4];
                    gl_FragColor = vec4(value, value, value, 1.0);
                }
            )",
            .preprocess_info = {},
        },
    };
    std::vector<wallpaper::ShaderCode> codes;
    const bool compiled = wallpaper::WPShaderParser::CompileToSpv(
        "packed-audio-spectrum-test",
        std::span<wallpaper::WPShaderUnit>(units.data(), units.size()),
        codes,
        vfs,
        &shaderInfo,
        std::span<const wallpaper::WPShaderTexInfo>());
    Require(compiled, "packed audio spectrum shader should compile through DXC");
    Require(codes.size() == units.size(), "compiled shader stage count mismatch");
    Require(! codes[0].empty() && ! codes[1].empty(), "compiled shader code must not be empty");
}

} // namespace

int main() {
    TestExactPackedIndex();
    TestGenericPackedIndex();
    TestOnlyAudioArraysAreRewritten();
    TestMalformedAccessStaysVisible();
    TestPackedAudioShaderCompiles();
    return 0;
}
