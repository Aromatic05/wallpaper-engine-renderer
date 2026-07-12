#include "WPShaderParser.hpp"
#include "WPShaderParserTestHooks.hpp"

#include "fs/IBinaryStream.h"
#include "utils/Logging.h"
#include "WPJson.hpp"

#include "wpscene/WPUniform.h"
#include "fs/VFS.h"
#include "utils/Sha.hpp"
#include "utils/String.h"
#include "WPCommon.hpp"

#include "vulkan/ShaderComp.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>

static constexpr std::string_view SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__" };

#define SHADER_DIR    "spvs01"
#define SHADER_SUFFIX "spvs"
#define SHADER_META_DIR "pre-shaders01"
#define SHADER_META_SUFFIX "wpmeta"
#define SHADER_SRC_DIR "prepared-shaders02"
#define SHADER_SRC_SUFFIX "wpsrc"

static constexpr int              kPreparedShaderSourceVersion { 4 };
static constexpr std::string_view kPreparedShaderPipelineKey {
    "prepared-shader-v19-dxc-interface-sanitize\n"
};

using namespace wallpaper;

namespace
{
inline void ApplyKnownIncludeSourceFixes(std::string_view include_name, std::string& source) {
    if (include_name != "common_perspective.h" ||
        source.find("_ww_squareToQuadColumnVectorCompat") != std::string::npos) {
        return;
    }

    // `common_perspective.h` is authored for WE's HLSL row-vector helper contract:
    // `squareToQuad()` builds a projective matrix with HLSL row indexing, `inverse(mat3)` returns a
    // matrix built from scalar constructor arguments, and callers use `mul(vec, matrix)`. Hanabi's
    // DXC bridge swaps authored `mul(vec, matrix)` into column-vector math, while the GLSL matrix
    // constructor bridge below now also converts scalar `mat3(...)` constructors to GLSL column
    // semantics. Those two bridges together mean the stock HLSL `inverse(mat3)` already produces
    // the transposed inverse needed by column-vector multiplication. Returning a further
    // `transpose(squareToQuad(...))` here double-transposes EffectPerspectiveUV and makes direct
    // draw perspective effects disappear, so this include-level shim now preserves the row-form
    // `squareToQuad()` output and lets `inverse()` perform the single required conversion.
    source.append(R"(

#if HLSL
#ifndef WW_COMMON_PERSPECTIVE_COLUMN_VECTOR_COMPAT
#define WW_COMMON_PERSPECTIVE_COLUMN_VECTOR_COMPAT 1
mat3 _ww_squareToQuadColumnVectorCompat(vec2 p0, vec2 p1, vec2 p2, vec2 p3) {
    return squareToQuad(p0, p1, p2, p3);
}
#endif
#define squareToQuad _ww_squareToQuadColumnVectorCompat
#endif
)");
}

inline std::string LoadGlslInclude(fs::VFS& vfs, const std::string& input) {
    std::string::size_type pos = 0;
    std::string            output;
    std::string::size_type linePos = std::string::npos;

    while (linePos = input.find("#include", pos), linePos != std::string::npos) {
        auto lineEnd  = input.find_first_of('\n', linePos);
        auto lineSize = lineEnd - linePos;
        auto lineStr  = input.substr(linePos, lineSize);
        output.append(input.substr(pos, linePos - pos));

        auto inP         = lineStr.find_first_of('\"') + 1;
        auto inE         = lineStr.find_last_of('\"');
        auto includeName = lineStr.substr(inP, inE - inP);
        auto includeSrc  = fs::GetFileContent(vfs, "/assets/shaders/" + includeName);
        ApplyKnownIncludeSourceFixes(includeName, includeSrc);
        output.append("\n//-----include " + includeName + "\n");
        output.append(LoadGlslInclude(vfs, includeSrc));
        output.append("\n//-----include end\n");

        pos = lineEnd;
    }
    output.append(input.substr(pos));
    return output;
}

inline size_t SkipWhitespace(std::string_view source, size_t pos) {
    while (pos < source.size() && std::isspace(static_cast<unsigned char>(source[pos]))) {
        pos++;
    }
    return pos;
}

inline char PrevNonWhitespace(std::string_view source, size_t pos) {
    while (pos > 0) {
        pos--;
        if (! std::isspace(static_cast<unsigned char>(source[pos]))) return source[pos];
    }
    return '\0';
}

inline bool LooksLikeObjectKeyAfterComma(std::string_view source, size_t pos) {
    pos = SkipWhitespace(source, pos);
    if (pos >= source.size() || source[pos] != '"') return false;

    bool saw_char { false };
    bool escaped { false };
    pos++;
    while (pos < source.size()) {
        const char ch = source[pos];
        if (escaped) {
            escaped  = false;
            saw_char = true;
            pos++;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            pos++;
            continue;
        }
        if (ch == '"') break;
        saw_char = true;
        pos++;
    }

    if (pos >= source.size() || ! saw_char) return false;
    pos = SkipWhitespace(source, pos + 1);
    return pos < source.size() && source[pos] == ':';
}

inline bool LooksLikeContainerClose(std::string_view source, size_t pos) {
    if (pos >= source.size() || (source[pos] != '}' && source[pos] != ']')) return false;

    pos = SkipWhitespace(source, pos + 1);
    return pos >= source.size() || source[pos] == ',' || source[pos] == '}' || source[pos] == ']';
}

inline std::string TruncateMetadataSnippet(std::string_view source) {
    constexpr size_t max_len { 120 };
    if (source.size() <= max_len) return std::string(source);
    return std::string(source.substr(0, max_len - 3)) + "...";
}

inline bool IsIdentifierStart(char ch) {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalpha(uch) || ch == '_';
}

inline bool IsIdentifierContinue(char ch) {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_';
}

inline size_t SkipIdentifier(std::string_view source, size_t pos) {
    while (pos < source.size() && IsIdentifierContinue(source[pos])) {
        pos++;
    }
    return pos;
}

inline size_t SkipBalanced(std::string_view source, size_t pos, char open_ch, char close_ch) {
    if (pos >= source.size() || source[pos] != open_ch) return std::string::npos;

    int depth { 0 };
    while (pos < source.size()) {
        if (source[pos] == open_ch) {
            depth++;
        } else if (source[pos] == close_ch) {
            depth--;
            if (depth == 0) return pos + 1;
        }
        pos++;
    }
    return std::string::npos;
}

inline bool ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
    if (from.empty()) return false;

    bool   changed { false };
    size_t pos { 0 };
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
        changed = true;
    }
    return changed;
}

inline std::string_view TrimLeadingHorizontalWhitespace(std::string_view source) {
    const auto pos = source.find_first_not_of(" \t\r");
    if (pos == std::string_view::npos) return {};
    return source.substr(pos);
}

inline bool StartsWithToken(std::string_view source, std::string_view token) {
    if (source.size() < token.size()) return false;
    if (source.substr(0, token.size()) != token) return false;
    return source.size() == token.size() ||
           ! IsIdentifierContinue(source[token.size()]);
}

inline void UpdateBraceDepth(std::string_view line, int& brace_depth, bool& in_block_comment) {
    bool in_string { false };
    bool escaped { false };
    char quote { '\0' };

    for (size_t i = 0; i < line.size(); i++) {
        const char ch = line[i];
        const char next = i + 1 < line.size() ? line[i + 1] : '\0';

        if (in_block_comment) {
            if (ch == '*' && next == '/') {
                in_block_comment = false;
                i++;
            }
            continue;
        }

        if (in_string) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == quote) in_string = false;
            continue;
        }

        if (ch == '/' && next == '/') break;
        if (ch == '/' && next == '*') {
            in_block_comment = true;
            i++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_string = true;
            quote     = ch;
            continue;
        }

        if (ch == '{') {
            brace_depth++;
        } else if (ch == '}' && brace_depth > 0) {
            brace_depth--;
        }
    }
}

inline size_t FindStatementTerminator(std::string_view text, size_t pos) {
    bool in_block_comment { false };
    bool in_string { false };
    bool escaped { false };
    char quote { '\0' };
    int  paren_depth { 0 };
    int  bracket_depth { 0 };

    while (pos < text.size()) {
        const char ch   = text[pos];
        const char next = pos + 1 < text.size() ? text[pos + 1] : '\0';

        if (in_block_comment) {
            if (ch == '*' && next == '/') {
                in_block_comment = false;
                pos += 2;
                continue;
            }
            pos++;
            continue;
        }

        if (in_string) {
            if (escaped) {
                escaped = false;
                pos++;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                pos++;
                continue;
            }
            if (ch == quote) in_string = false;
            pos++;
            continue;
        }

        if (ch == '/' && next == '/') {
            const auto line_end = text.find('\n', pos);
            return line_end == std::string_view::npos ? std::string::npos : line_end;
        }
        if (ch == '/' && next == '*') {
            in_block_comment = true;
            pos += 2;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_string = true;
            quote     = ch;
            pos++;
            continue;
        }

        if (ch == '(') paren_depth++;
        else if (ch == ')' && paren_depth > 0) paren_depth--;
        else if (ch == '[') bracket_depth++;
        else if (ch == ']' && bracket_depth > 0) bracket_depth--;
        else if (ch == ';' && paren_depth == 0 && bracket_depth == 0) return pos;

        pos++;
    }

    return std::string::npos;
}

struct DeclMatch {
    size_t      start { 0 };
    size_t      end { 0 };
    size_t      keep_prefix { 0 };
    std::string storage;
    std::string type;
    std::string name;
    std::string array;
};

inline std::optional<DeclMatch> TryParseDeclLine(
    std::string_view src,
    size_t line_start,
    std::initializer_list<std::string_view> storage_keywords) {
    size_t line_end = src.find('\n', line_start);
    if (line_end == std::string_view::npos) line_end = src.size();
    std::string_view line = src.substr(line_start, line_end - line_start);

    size_t pos = SkipWhitespace(line, 0);
    if (pos >= line.size() || ! IsIdentifierStart(line[pos])) return std::nullopt;

    const auto storage_end = SkipIdentifier(line, pos);
    const auto storage     = line.substr(pos, storage_end - pos);
    bool       matched_storage { false };
    for (auto keyword : storage_keywords) {
        if (storage == keyword) {
            matched_storage = true;
            break;
        }
    }
    if (! matched_storage) return std::nullopt;

    pos = SkipWhitespace(line, storage_end);
    if (pos >= line.size() || ! IsIdentifierStart(line[pos])) return std::nullopt;
    auto type_end = SkipIdentifier(line, pos);
    auto type     = line.substr(pos, type_end - pos);
    if (type == "highp" || type == "mediump" || type == "lowp") {
        pos = SkipWhitespace(line, type_end);
        if (pos >= line.size() || ! IsIdentifierStart(line[pos])) return std::nullopt;
        type_end = SkipIdentifier(line, pos);
        type     = line.substr(pos, type_end - pos);
    }

    pos = SkipWhitespace(line, type_end);
    if (pos >= line.size() || ! IsIdentifierStart(line[pos])) return std::nullopt;
    const auto name_end = SkipIdentifier(line, pos);
    const auto name     = line.substr(pos, name_end - pos);

    pos = SkipWhitespace(line, name_end);
    if (pos < line.size() && line[pos] == '.' &&
        (storage == "attribute" || storage == "varying" || storage == "in" || storage == "out")) {
        // Some Workshop effects export declarations such as `varying vec4 v_Size.xy;`. The suffix
        // is a stray swizzle on the declaration, while later `v_Size.xy` expressions are valid.
        // Treat the declaration as `v_Size` so the DXC bridge strips it and emits a valid interface.
        pos++;
        if (pos >= line.size() || ! IsIdentifierStart(line[pos])) return std::nullopt;
        const auto suffix_end = SkipIdentifier(line, pos);
        pos = SkipWhitespace(line, suffix_end);
    }

    std::string array;
    if (pos < line.size() && line[pos] == '[') {
        const auto array_begin = pos;
        pos                    = SkipBalanced(line, pos, '[', ']');
        if (pos == std::string::npos || pos <= array_begin) return std::nullopt;
        array = std::string(line.substr(array_begin, pos - array_begin));
        pos   = SkipWhitespace(line, pos);
    }

    if (pos >= line.size() || line[pos] != ';') return std::nullopt;
    return DeclMatch { .start       = line_start,
                       .end         = line_start + pos + 1,
                       .keep_prefix = 0,
                       .storage     = std::string(storage),
                       .type        = std::string(type),
                       .name        = std::string(name),
                       .array       = std::move(array) };
}

template<typename Fn>
inline void ForEachDeclLine(std::string_view src,
                            std::initializer_list<std::string_view> storage_keywords,
                            Fn&& fn) {
    size_t line_start { 0 };
    while (line_start <= src.size()) {
        if (auto match = TryParseDeclLine(src, line_start, storage_keywords); match.has_value()) {
            auto out = *match;
            if (line_start > 0 && src[line_start - 1] == '\n') {
                out.start       = line_start - 1;
                out.keep_prefix = 1;
            }
            fn(out);
        }

        const auto line_end = src.find('\n', line_start);
        if (line_end == std::string_view::npos) break;
        line_start = line_end + 1;
    }
}

inline bool IsSamplerType(std::string_view type) {
    return type == "sampler2D" || type == "sampler3D" || type == "samplerCube" ||
        type == "sampler2DComparison" || type == "sampler2DShadow";
}

inline std::string ToHLSLType(std::string_view type) {
    if (type == "vec2") return "float2";
    if (type == "vec3") return "float3";
    if (type == "vec4") return "float4";
    if (type == "ivec2") return "int2";
    if (type == "ivec3") return "int3";
    if (type == "ivec4") return "int4";
    if (type == "uvec2") return "uint2";
    if (type == "uvec3") return "uint3";
    if (type == "uvec4") return "uint4";
    if (type == "bvec2") return "bool2";
    if (type == "bvec3") return "bool3";
    if (type == "bvec4") return "bool4";
    if (type == "mat2" || type == "mat2x2") return "float2x2";
    if (type == "mat3" || type == "mat3x3") return "float3x3";
    if (type == "mat4" || type == "mat4x4") return "float4x4";
    // WE uses GLSL matrix type names (`matCxR`: C columns by R rows) inside an HLSL-like shader
    // dialect. HLSL spells matrices as `floatRxC`, so only the type-name indices are swapped here;
    // multiplication semantics are handled once in the DXC prologue instead of by expression-level
    // source transformations.
    if (type == "mat2x3") return "float3x2";
    if (type == "mat2x4") return "float4x2";
    if (type == "mat3x2") return "float2x3";
    if (type == "mat3x4") return "float4x3";
    if (type == "mat4x2") return "float2x4";
    if (type == "mat4x3") return "float3x4";
    return std::string(type);
}

inline std::string MakeDeclLine(const DeclMatch& match) {
    return match.storage + " " + match.type + " " + match.name + match.array + ";";
}

inline bool LineDefinesMacro(std::string_view src, std::size_t line_start,
                             std::string_view macro_name) {
    const auto line_end = src.find('\n', line_start);
    const auto line_len = line_end == std::string_view::npos ? src.size() - line_start
                                                             : line_end - line_start;
    auto line = src.substr(line_start, line_len);
    line      = TrimLeadingHorizontalWhitespace(line);
    if (line.rfind("#define", 0) != 0) return false;

    size_t pos = std::char_traits<char>::length("#define");
    pos        = SkipWhitespace(line, pos);
    const auto ident_end = SkipIdentifier(line, pos);
    if (ident_end == pos) return false;
    return line.substr(pos, ident_end - pos) == macro_name;
}

inline std::string UndefBeforeUserMacroDefines(std::string_view src, std::string_view macro_name) {
    bool changed = false;
    std::string out;
    out.reserve(src.size() + 64);

    size_t line_start { 0 };
    while (line_start < src.size()) {
        const auto line_end = src.find('\n', line_start);
        if (LineDefinesMacro(src, line_start, macro_name)) {
            out += "#ifdef ";
            out += macro_name;
            out += "\n#undef ";
            out += macro_name;
            out += "\n#endif\n";
            changed = true;
        }
        if (line_end == std::string_view::npos) {
            out.append(src, line_start, src.size() - line_start);
            break;
        }
        out.append(src, line_start, line_end - line_start);
        out.push_back('\n');
        line_start = line_end + 1;
    }

    return changed ? out : std::string(src);
}

inline bool IsAudioSpectrumName(std::string_view name) {
    return name == "g_AudioSpectrum16Left" || name == "g_AudioSpectrum16Right" ||
           name == "g_AudioSpectrum32Left" || name == "g_AudioSpectrum32Right" ||
           name == "g_AudioSpectrum64Left" || name == "g_AudioSpectrum64Right";
}

inline size_t SkipShaderTrivia(std::string_view source, size_t pos) {
    for (;;) {
        pos = SkipWhitespace(source, pos);
        if (pos + 1 >= source.size() || source[pos] != '/') return pos;
        if (source[pos + 1] == '/') {
            const auto line_end = source.find('\n', pos + 2);
            if (line_end == std::string_view::npos) return source.size();
            pos = line_end + 1;
            continue;
        }
        if (source[pos + 1] != '*') return pos;
        const auto close = source.find("*/", pos + 2);
        if (close == std::string_view::npos) return source.size();
        pos = close + 2;
    }
}

inline size_t FindShaderBracketEnd(std::string_view source, size_t open_pos) {
    if (open_pos >= source.size() || source[open_pos] != '[') return std::string::npos;

    int  depth { 0 };
    bool in_block_comment { false };
    bool in_string { false };
    bool escaped { false };
    char quote { '\0' };
    for (size_t pos = open_pos; pos < source.size(); pos++) {
        const char ch = source[pos];
        const char next = pos + 1 < source.size() ? source[pos + 1] : '\0';
        if (in_block_comment) {
            if (ch == '*' && next == '/') {
                in_block_comment = false;
                pos++;
            }
            continue;
        }
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == quote) {
                in_string = false;
            }
            continue;
        }
        if (ch == '/' && next == '/') {
            const auto line_end = source.find('\n', pos + 2);
            if (line_end == std::string_view::npos) return std::string::npos;
            pos = line_end;
            continue;
        }
        if (ch == '/' && next == '*') {
            in_block_comment = true;
            pos++;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            in_string = true;
            quote = ch;
            continue;
        }
        if (ch == '[') {
            depth++;
        } else if (ch == ']') {
            depth--;
            if (depth == 0) return pos + 1;
        }
    }
    return std::string::npos;
}

inline std::string_view TrimShaderExpression(std::string_view expression) {
    const auto begin = expression.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const auto end = expression.find_last_not_of(" \t\r\n");
    return expression.substr(begin, end - begin + 1);
}

inline std::optional<std::string_view> ParsePackedAudioIndexTerm(std::string_view expression,
                                                                 char expected_operator) {
    expression = TrimShaderExpression(expression);
    if (expression.empty() || ! IsIdentifierStart(expression.front())) return std::nullopt;
    const auto identifier_end = SkipIdentifier(expression, 0);
    const auto identifier = expression.substr(0, identifier_end);
    auto pos = SkipWhitespace(expression, identifier_end);
    if (pos >= expression.size() || expression[pos] != expected_operator) return std::nullopt;
    pos = SkipWhitespace(expression, pos + 1);
    if (pos >= expression.size() || expression[pos] != '4') return std::nullopt;
    pos = SkipWhitespace(expression, pos + 1);
    if (pos != expression.size()) return std::nullopt;
    return identifier;
}

inline std::string FlattenPackedAudioIndex(std::string_view group, std::string_view component) {
    const auto group_identifier = ParsePackedAudioIndexTerm(group, '/');
    const auto component_identifier = ParsePackedAudioIndexTerm(component, '%');
    if (group_identifier && component_identifier && *group_identifier == *component_identifier) {
        return "(int)(" + std::string(*group_identifier) + ")";
    }

    std::string result;
    result.reserve(group.size() + component.size() + 32);
    result += "((int)(";
    result.append(group);
    result += ") * 4 + (int)(";
    result.append(component);
    result += "))";
    return result;
}

inline std::string NormalizePackedAudioSpectrumAccessImpl(std::string_view source) {
    std::string output;
    output.reserve(source.size());
    size_t copied { 0 };
    size_t pos { 0 };
    bool changed { false };
    bool in_block_comment { false };
    bool in_string { false };
    bool escaped { false };
    char quote { '\0' };

    while (pos < source.size()) {
        const char ch = source[pos];
        const char next = pos + 1 < source.size() ? source[pos + 1] : '\0';
        if (in_block_comment) {
            if (ch == '*' && next == '/') {
                in_block_comment = false;
                pos += 2;
            } else {
                pos++;
            }
            continue;
        }
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == quote) {
                in_string = false;
            }
            pos++;
            continue;
        }
        if (ch == '/' && next == '/') {
            const auto line_end = source.find('\n', pos + 2);
            pos = line_end == std::string_view::npos ? source.size() : line_end + 1;
            continue;
        }
        if (ch == '/' && next == '*') {
            in_block_comment = true;
            pos += 2;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            in_string = true;
            quote = ch;
            pos++;
            continue;
        }
        if (! IsIdentifierStart(ch)) {
            pos++;
            continue;
        }

        const auto identifier_end = SkipIdentifier(source, pos);
        const auto identifier = source.substr(pos, identifier_end - pos);
        if (! IsAudioSpectrumName(identifier)) {
            pos = identifier_end;
            continue;
        }

        const auto first_open = SkipShaderTrivia(source, identifier_end);
        const auto first_end = FindShaderBracketEnd(source, first_open);
        const auto second_open = first_end == std::string_view::npos
            ? std::string_view::npos
            : SkipShaderTrivia(source, first_end);
        const auto second_end = second_open == std::string_view::npos
            ? std::string_view::npos
            : FindShaderBracketEnd(source, second_open);
        if (first_end == std::string_view::npos || second_end == std::string_view::npos) {
            pos = identifier_end;
            continue;
        }

        const auto group = source.substr(first_open + 1, first_end - first_open - 2);
        const auto component = source.substr(second_open + 1, second_end - second_open - 2);
        output.append(source, copied, pos - copied);
        output.append(identifier);
        output.push_back('[');
        output += FlattenPackedAudioIndex(group, component);
        output.push_back(']');
        copied = second_end;
        pos = second_end;
        changed = true;
    }

    if (! changed) return std::string(source);
    output.append(source, copied, source.size() - copied);
    return output;
}

struct IODecl {
    char        storage { 'v' };
    std::string type;
    std::string name;
    std::string array;
};

struct SamplerDecl {
    std::string sampler_type;
    std::string name;
};

inline char StorageCharFor(std::string_view storage) {
    if (storage == "attribute") return 'a';
    if (storage == "in") return 'i';
    if (storage == "out") return 'o';
    return 'v';
}

inline std::optional<IODecl> ParseIODecl(const std::string& line) {
    size_t start = 0;
    while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) start++;
    auto match = TryParseDeclLine(line, start, { "attribute", "varying", "in", "out" });
    if (! match.has_value()) return std::nullopt;
    return IODecl { .storage = StorageCharFor(match->storage),
                    .type    = match->type,
                    .name    = match->name,
                    .array   = match->array };
}

inline void RecordDxcPreprocessorInfo(const std::string& src, ShaderType stage,
                                      WPPreprocessorInfo& process_info) {
    process_info.input.clear();
    process_info.output.clear();
    process_info.uniforms.clear();
    process_info.active_tex_slots.clear();

    ForEachDeclLine(src, { "attribute", "varying", "in", "out" }, [&](const DeclMatch& match) {
        const bool is_input = match.storage == "attribute" ||
            (match.storage == "varying" && stage == ShaderType::FRAGMENT) ||
            (match.storage == "in" && stage == ShaderType::GEOMETRY);
        const auto line = MakeDeclLine(match);
        if (is_input) process_info.input[match.name] = line;
        else process_info.output[match.name] = line;
    });

    ForEachDeclLine(src, { "uniform" }, [&](const DeclMatch& match) {
        if (IsSamplerType(match.type)) {
            constexpr std::string_view prefix { "g_Texture" };
            if (match.name.size() > prefix.size() &&
                std::string_view(match.name).substr(0, prefix.size()) == prefix) {
                uint slot { 0 };
                const auto number = std::string_view(match.name).substr(prefix.size());
                auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), slot);
                if (ec == std::errc() && ptr == number.data() + number.size()) {
                    process_info.active_tex_slots.insert(slot);
                }
            }
            return;
        }
        process_info.uniforms[match.name] = ToHLSLType(match.type) + match.array;
    });
}

inline std::pair<std::vector<IODecl>, std::string> ScanAndStripIO(const std::string& src) {
    std::vector<IODecl> decls;
    std::string         out;
    out.reserve(src.size());
    size_t cursor { 0 };
    ForEachDeclLine(src, { "attribute", "varying", "in", "out" }, [&](const DeclMatch& match) {
        out.append(src, cursor, match.start - cursor);
        out.append(src, match.start, match.keep_prefix);
        cursor = match.end;
        decls.push_back(IODecl { .storage = StorageCharFor(match.storage),
                                 .type    = match.type,
                                 .name    = match.name,
                                 .array   = match.array });
    });
    out.append(src, cursor, std::string::npos);
    return { std::move(decls), std::move(out) };
}

inline std::pair<std::vector<SamplerDecl>, std::string> ScanAndStripSamplers(
    const std::string& src) {
    std::vector<SamplerDecl> decls;
    std::string              out;
    out.reserve(src.size());
    size_t cursor { 0 };
    ForEachDeclLine(src, { "uniform" }, [&](const DeclMatch& match) {
        if (! IsSamplerType(match.type)) return;
        out.append(src, cursor, match.start - cursor);
        out.append(src, match.start, match.keep_prefix);
        cursor = match.end;
        decls.push_back(SamplerDecl { .sampler_type = match.type, .name = match.name });
    });
    out.append(src, cursor, std::string::npos);
    return { std::move(decls), std::move(out) };
}

inline std::string StripUniforms(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    size_t cursor { 0 };
    ForEachDeclLine(src, { "uniform" }, [&](const DeclMatch& match) {
        out.append(src, cursor, match.start - cursor);
        out.append(src, match.start, match.keep_prefix);
        cursor = match.end;
    });
    out.append(src, cursor, std::string::npos);
    return out;
}

inline size_t ParseArrayCount(std::string_view array) {
    if (array.size() < 3 || array.front() != '[' || array.back() != ']') return 1;
    const auto inner = array.substr(1, array.size() - 2);
    size_t     count { 0 };
    for (const char ch : inner) {
        if (std::isspace(static_cast<unsigned char>(ch))) continue;
        if (ch < '0' || ch > '9') return 1;
        count = count * 10 + static_cast<size_t>(ch - '0');
    }
    return count == 0 ? 1 : count;
}

struct Std140Layout {
    size_t align { 16 };
    size_t size { 16 };
};

inline Std140Layout Std140Base(std::string_view hlsl_type) {
    if (hlsl_type == "float" || hlsl_type == "int" || hlsl_type == "uint" ||
        hlsl_type == "bool") return { 4, 4 };
    if (hlsl_type == "float2" || hlsl_type == "int2" || hlsl_type == "uint2" ||
        hlsl_type == "bool2") return { 8, 8 };
    if (hlsl_type == "float3" || hlsl_type == "int3" || hlsl_type == "uint3" ||
        hlsl_type == "bool3") return { 16, 12 };
    if (hlsl_type == "float4" || hlsl_type == "int4" || hlsl_type == "uint4" ||
        hlsl_type == "bool4") return { 16, 16 };
    if (hlsl_type.size() == 8 && hlsl_type.substr(0, 5) == "float" &&
        hlsl_type[6] == 'x' && hlsl_type[5] >= '2' && hlsl_type[5] <= '4' &&
        hlsl_type[7] >= '2' && hlsl_type[7] <= '4') {
        return { 16, static_cast<size_t>(hlsl_type[7] - '0') * 16 };
    }
    return { 16, 16 };
}

inline std::string EmitCBufferStd140(const Map<std::string, std::string>& uniforms) {
    if (uniforms.empty()) return {};

    std::string out;
    out += "[[vk::binding(0, 0)]] cbuffer ww_Uniforms {\n";
    size_t offset { 0 };
    for (const auto& [name, type_with_array] : uniforms) {
        std::string type = type_with_array;
        std::string array;
        if (const auto array_pos = type_with_array.find('['); array_pos != std::string::npos) {
            type  = type_with_array.substr(0, array_pos);
            array = type_with_array.substr(array_pos);
        }

        const auto hlsl_type = ToHLSLType(type);
        const auto count     = ParseArrayCount(array);
        const auto layout    = Std140Base(hlsl_type);
        const auto align     = count > 1 ? size_t(16) : layout.align;
        const auto size      = count > 1 ? ((layout.size + 15) & ~size_t(15)) * count
                                         : layout.size;
        offset               = (offset + align - 1) & ~(align - 1);

        const size_t reg  = offset / 16;
        const size_t comp = (offset % 16) / 4;
        const bool is_matrix = hlsl_type.rfind("float", 0) == 0 && hlsl_type.find('x') != std::string::npos;

        out += "    ";
        if (is_matrix) out += "column_major ";
        out += hlsl_type + " " + name + array + " : packoffset(c" + std::to_string(reg);
        if (comp != 0) {
            out += ".";
            out.push_back("xyzw"[comp]);
        }
        out += ");\n";
        offset += size;
    }
    out += "};\n";
    return out;
}

inline std::string EmitVSFSStruct(std::string_view name, std::vector<IODecl> decls,
                                  bool include_sv_position) {
    decls.erase(std::remove_if(decls.begin(), decls.end(), [](const IODecl& decl) {
                    return decl.name == "gl_Position" || decl.name == "_ww_sv_position";
                }),
                decls.end());
    std::sort(decls.begin(), decls.end(), [](const IODecl& a, const IODecl& b) {
        return a.name < b.name;
    });

    std::string out;
    out += "struct ";
    out += name;
    out += " {\n";
    if (include_sv_position) out += "    float4 _ww_sv_position : SV_Position;\n";
    for (const auto& decl : decls) {
        out += "    " + ToHLSLType(decl.type) + " " + decl.name + decl.array + " : " +
            decl.name + ";\n";
    }
    out += "};\n";
    return out;
}

struct DxcSynthOutput {
    std::string pre;
    std::string post;
};

inline DxcSynthOutput SynthesizeDxcEntry(ShaderType stage, std::vector<IODecl> attrs,
                                         std::vector<IODecl> varyings) {
    DxcSynthOutput output;

    auto drop_position = [](std::vector<IODecl>& decls) {
        decls.erase(std::remove_if(decls.begin(), decls.end(), [](const IODecl& decl) {
                        return decl.name == "gl_Position" || decl.name == "_ww_sv_position";
                    }),
                    decls.end());
    };
    drop_position(attrs);
    drop_position(varyings);

    auto by_name = [](const IODecl& a, const IODecl& b) { return a.name < b.name; };
    std::sort(attrs.begin(), attrs.end(), by_name);
    std::sort(varyings.begin(), varyings.end(), by_name);

    output.pre += "\n// === auto-generated WE stage globals ===\n";
    for (const auto& decl : attrs) {
        output.pre += "static " + ToHLSLType(decl.type) + " " + decl.name + decl.array + ";\n";
    }
    for (const auto& decl : varyings) {
        output.pre += "static " + ToHLSLType(decl.type) + " " + decl.name + decl.array + ";\n";
    }

    output.post += "\n// === auto-generated WE HLSL entry point ===\n";
    if (stage == ShaderType::VERTEX) {
        output.post += EmitVSFSStruct("WW_VSIn", attrs, false);
        output.post += EmitVSFSStruct("WW_VSOut", varyings, true);
        output.post += "WW_VSOut main_vs(WW_VSIn _ww_in) {\n";
        for (const auto& attr : attrs) {
            output.post += "    " + attr.name + " = _ww_in." + attr.name + ";\n";
        }
        output.post += "    shader_main();\n";
        output.post += "    WW_VSOut _ww_out;\n";
        output.post += "    _ww_out._ww_sv_position = gl_Position;\n";
        for (const auto& varying : varyings) {
            output.post += "    _ww_out." + varying.name + " = " + varying.name + ";\n";
        }
        output.post += "    return _ww_out;\n";
        output.post += "}\n";
    } else if (stage == ShaderType::FRAGMENT) {
        output.post += EmitVSFSStruct("WW_PSIn", varyings, true);
        output.post += "float4 main_ps(WW_PSIn _ww_in) : SV_Target0 {\n";
        output.post += "    gl_FragCoord = _ww_in._ww_sv_position;\n";
        for (const auto& varying : varyings) {
            output.post += "    " + varying.name + " = _ww_in." + varying.name + ";\n";
        }
        output.post += "    shader_main();\n";
        output.post += "    return glOutColor;\n";
        output.post += "}\n";
    }
    return output;
}

inline const char* HLSLSamplerType(std::string_view sampler_type) {
    if (sampler_type == "sampler3D") return "Texture3D<float4>";
    if (sampler_type == "samplerCube") return "TextureCube<float4>";
    if (sampler_type == "sampler2DComparison" || sampler_type == "sampler2DShadow") {
        return "Texture2D<float>";
    }
    return "Texture2D<float4>";
}

inline const char* HLSLSamplerStateType(std::string_view sampler_type) {
    if (sampler_type == "sampler2DComparison" || sampler_type == "sampler2DShadow") {
        return "SamplerComparisonState";
    }
    return "SamplerState";
}

inline std::string EmitSamplerBlock(const std::vector<SamplerDecl>& samplers) {
    std::string      out;
    Set<std::string> seen;
    size_t           binding { 1 };
    for (const auto& sampler : samplers) {
        if (! seen.insert(sampler.name).second) continue;
        out += "[[vk::combinedImageSampler]][[vk::binding(" + std::to_string(binding) +
            ", 0)]] ";
        out += HLSLSamplerType(sampler.sampler_type);
        out += " " + sampler.name + ";\n";
        out += "[[vk::combinedImageSampler]][[vk::binding(" + std::to_string(binding) +
            ", 0)]] ";
        out += HLSLSamplerStateType(sampler.sampler_type);
        out += " " + sampler.name + "_ww_sampler;\n";
        binding++;
    }
    return out;
}

inline std::string StripDxcLineDirectives(std::string_view src) {
    std::string out;
    out.reserve(src.size());

    size_t pos { 0 };
    while (pos < src.size()) {
        const auto line_end = src.find('\n', pos);
        const auto line_len = line_end == std::string_view::npos ? src.size() - pos
                                                                 : line_end - pos;
        const auto line     = src.substr(pos, line_len);
        const auto trimmed  = TrimLeadingHorizontalWhitespace(line);
        if (! trimmed.empty() && trimmed.rfind("#line", 0) == 0) {
            out.append(line.substr(0, line.size() - trimmed.size()));
            out.append("// ");
            out.append(line);
        } else {
            out.append(line);
        }

        if (line_end == std::string_view::npos) break;
        out.push_back('\n');
        pos = line_end + 1;
    }
    return out;
}

inline bool IsPrecisionQualifier(std::string_view token) {
    return token == "highp" || token == "mediump" || token == "lowp";
}

inline bool LooksLikeTopLevelConstGlobal(std::string_view trimmed_line) {
    if (! StartsWithToken(trimmed_line, "const")) return false;

    auto pos = SkipWhitespace(trimmed_line, std::char_traits<char>::length("const"));
    while (pos < trimmed_line.size() && IsIdentifierStart(trimmed_line[pos])) {
        const auto token_end = SkipIdentifier(trimmed_line, pos);
        const auto token     = trimmed_line.substr(pos, token_end - pos);
        if (! IsPrecisionQualifier(token)) break;
        pos = SkipWhitespace(trimmed_line, token_end);
    }

    if (pos >= trimmed_line.size() || ! IsIdentifierStart(trimmed_line[pos])) return false;
    const auto type_end = SkipIdentifier(trimmed_line, pos);
    const auto type     = trimmed_line.substr(pos, type_end - pos);
    if (type == "struct" || type == "class") return false;

    pos = SkipWhitespace(trimmed_line, type_end);
    if (pos >= trimmed_line.size() || ! IsIdentifierStart(trimmed_line[pos])) return false;

    const auto name_end = SkipIdentifier(trimmed_line, pos);
    pos                 = SkipWhitespace(trimmed_line, name_end);
    if (pos < trimmed_line.size() && trimmed_line[pos] == '[') {
        pos = SkipBalanced(trimmed_line, pos, '[', ']');
        if (pos == std::string::npos) return false;
        pos = SkipWhitespace(trimmed_line, pos);
    }

    if (pos >= trimmed_line.size() || trimmed_line[pos] != '=') return false;
    return FindStatementTerminator(trimmed_line, 0) != std::string::npos;
}

inline std::string MakeTopLevelConstGlobalsStaticForDxc(std::string_view src,
                                                        size_t& updates) {
    std::string out;
    out.reserve(src.size() + 32);
    updates = 0;

    int  brace_depth { 0 };
    bool in_block_comment { false };

    size_t pos { 0 };
    while (pos < src.size()) {
        const auto line_end = src.find('\n', pos);
        const auto line_len = line_end == std::string_view::npos ? src.size() - pos
                                                                 : line_end - pos;
        const auto line = src.substr(pos, line_len);

        const bool can_update = brace_depth == 0 && ! in_block_comment;
        const auto trimmed    = TrimLeadingHorizontalWhitespace(line);
        if (can_update && LooksLikeTopLevelConstGlobal(trimmed)) {
            // DXC treats non-static HLSL globals as externally visible shader globals and lowers
            // them into a `$Globals` cbuffer. Wallpaper Engine's GLSL-style top-level `const`
            // declarations are compile-time constants instead, so marking only those top-level
            // declarations `static const` preserves the authored meaning and keeps binding 0
            // reserved for the generated `ww_Uniforms` block.
            out.append(line.substr(0, line.size() - trimmed.size()));
            out.append("static ");
            out.append(trimmed);
            updates++;
        } else {
            out.append(line);
        }

        UpdateBraceDepth(line, brace_depth, in_block_comment);

        if (line_end == std::string_view::npos) break;
        out.push_back('\n');
        pos = line_end + 1;
    }
    return out;
}

inline std::string BuildDxcWePrologue(const Combos& combos, ShaderType stage) {
    std::string out = R"(// auto-generated Wallpaper Engine to HLSL prologue
#pragma pack_matrix(column_major)
#define HLSL 1
#define GLSL 0
// HLSL_SM30 is a legacy Direct3D 9/SM3 shader path in Wallpaper Engine's stock sources.
// Those branches expect single-channel textures to be sampled from alpha, but DXC emits SPIR-V
// for Vulkan where VK_FORMAT_R8_UNORM samples expose coverage in red. Leave HLSL_SM30 undefined
// so R8 particle masks, font atlases, and other single-channel assets use the Vulkan/GL swizzle.
#define highp
#define mediump
#define lowp

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define ivec2 int2
#define ivec3 int3
#define ivec4 int4
#define uvec2 uint2
#define uvec3 uint3
#define uvec4 uint4
#define bvec2 bool2
#define bvec3 bool3
#define bvec4 bool4
typedef float2x2 mat2;
typedef float3x3 mat3;
typedef float4x4 mat4;
typedef float2x2 mat2x2;
typedef float3x3 mat3x3;
typedef float4x4 mat4x4;
// GLSL `matCxR` declares C columns x R rows. HLSL `floatRxC` declares R rows x C columns, so
// rectangular aliases swap the type-name indices while the `_ww_mul` overloads below preserve
// WE's authored vector/matrix math against Hanabi's column-vector transform upload contract.
typedef float3x2 mat2x3;
typedef float4x2 mat2x4;
typedef float2x3 mat3x2;
typedef float4x3 mat3x4;
typedef float2x4 mat4x2;
typedef float3x4 mat4x3;

#define CAST2(x) ((float2)(x))
#define CAST3(x) ((float3)(x))
#define CAST4(x) ((float4)(x))
#define CAST3X3(x) ((float3x3)(x))

#define mix(a,b,t) lerp((a),(b),(t))
#define fract frac
#define atan(a,b) atan2((a),(b))
#define dFdx ddx
#define dFdy(x) (-ddy(x))

float2 wpFlipSampleY(float2 uv) { return float2(uv.x, 1.0 - uv.y); }
float3 wpFlipSampleY(float3 uv) { return float3(uv.x, 1.0 - uv.y, uv.z); }
float4 wpFlipSampleY(float4 uv) { return float4(uv.x, 1.0 - uv.y, uv.zw); }

// HLSL fills matrix constructor arguments by rows, while GLSL and Wallpaper Engine's stock
// GLSL-style shaders fill them by columns. That difference is invisible for uniform matrices but
// fatal for locally-built bases such as `mat3(tangent, bitangent, normal)`: the existing `mul`
// bridge assumes those vectors are columns, so a raw HLSL constructor transposes tangent space and
// turns normal-mapped lighting almost black on receiver surfaces. Keep `matN` usable as a type
// through typedefs above, and route only constructor calls through function-like macros.
mat2 _ww_mat2(float s) {
    return float2x2(s, 0.0,
                    0.0, s);
}
mat2 _ww_mat2(float2 c0, float2 c1) {
    return transpose(float2x2(c0, c1));
}
mat2 _ww_mat2(float m00, float m01,
              float m10, float m11) {
    return transpose(float2x2(m00, m01,
                              m10, m11));
}
mat3 _ww_mat3(float s) {
    return float3x3(s, 0.0, 0.0,
                    0.0, s, 0.0,
                    0.0, 0.0, s);
}
mat3 _ww_mat3(float3 c0, float3 c1, float3 c2) {
    return transpose(float3x3(c0, c1, c2));
}
mat3 _ww_mat3(float m00, float m01, float m02,
              float m10, float m11, float m12,
              float m20, float m21, float m22) {
    return transpose(float3x3(m00, m01, m02,
                              m10, m11, m12,
                              m20, m21, m22));
}
mat4 _ww_mat4(float s) {
    return float4x4(s, 0.0, 0.0, 0.0,
                    0.0, s, 0.0, 0.0,
                    0.0, 0.0, s, 0.0,
                    0.0, 0.0, 0.0, s);
}
mat4 _ww_mat4(float4 c0, float4 c1, float4 c2, float4 c3) {
    return transpose(float4x4(c0, c1, c2, c3));
}
mat4 _ww_mat4(float m00, float m01, float m02, float m03,
              float m10, float m11, float m12, float m13,
              float m20, float m21, float m22, float m23,
              float m30, float m31, float m32, float m33) {
    return transpose(float4x4(m00, m01, m02, m03,
                              m10, m11, m12, m13,
                              m20, m21, m22, m23,
                              m30, m31, m32, m33));
}
#define mat2(...) _ww_mat2(__VA_ARGS__)
#define mat2x2(...) _ww_mat2(__VA_ARGS__)
#define mat3(...) _ww_mat3(__VA_ARGS__)
#define mat3x3(...) _ww_mat3(__VA_ARGS__)
#define mat4(...) _ww_mat4(__VA_ARGS__)
#define mat4x4(...) _ww_mat4(__VA_ARGS__)

#ifndef WW_USER_MOD
float  mod(float  a, float  b) { return a - b * floor(a / b); }
float2 mod(float2 a, float2 b) { return a - b * floor(a / b); }
float3 mod(float3 a, float3 b) { return a - b * floor(a / b); }
float4 mod(float4 a, float4 b) { return a - b * floor(a / b); }
float2 mod(float2 a, float  b) { return a - b * floor(a / b); }
float3 mod(float3 a, float  b) { return a - b * floor(a / b); }
float4 mod(float4 a, float  b) { return a - b * floor(a / b); }
#endif

// WE shaders commonly write HLSL-style `mul(vec, matrix)`, but the scene runtime owns transforms as
// GLSL-style column-vector matrices (`M * v`) and uploads Eigen's column-major memory directly.
// Native DXC would keep row-vector source semantics and lower `mul(vec, matrix)` to a transposed
// projection relative to that upload contract. These overloads are the renderer-side language
// bridge: they swap only well-typed multiply operations, avoiding the old regex/expression transform
// chain while keeping local projective matrices, particle rotations, MVP transforms, and bone
// matrices in one consistent WE semantic model.
float2   _ww_mul(float2   v, float2x2 M) { return mul(M, v); }
float3   _ww_mul(float3   v, float3x3 M) { return mul(M, v); }
float4   _ww_mul(float4   v, float4x4 M) { return mul(M, v); }
float2x2 _ww_mul(float2x2 A, float2x2 B) { return mul(B, A); }
float3x3 _ww_mul(float3x3 A, float3x3 B) { return mul(B, A); }
float4x4 _ww_mul(float4x4 A, float4x4 B) { return mul(B, A); }
float2   _ww_mul(float2x2 M, float2   v) { return mul(v, M); }
float3   _ww_mul(float3x3 M, float3   v) { return mul(v, M); }
float4   _ww_mul(float4x4 M, float4   v) { return mul(v, M); }
// Rectangular GLSL aliases become HLSL `floatRxC`. Route both authored operand orders through
// native `mul(M, v)`, which matches GLSL's `M * v` result shape and avoids DXC's truncating
// implicit-conversion candidates.
float3   _ww_mul(float2   v, float3x2 M) { return mul(M, v); }
float3   _ww_mul(float3x2 M, float2   v) { return mul(M, v); }
float4   _ww_mul(float2   v, float4x2 M) { return mul(M, v); }
float4   _ww_mul(float4x2 M, float2   v) { return mul(M, v); }
float2   _ww_mul(float3   v, float2x3 M) { return mul(M, v); }
float2   _ww_mul(float2x3 M, float3   v) { return mul(M, v); }
float4   _ww_mul(float3   v, float4x3 M) { return mul(M, v); }
float4   _ww_mul(float4x3 M, float3   v) { return mul(M, v); }
float2   _ww_mul(float4   v, float2x4 M) { return mul(M, v); }
float2   _ww_mul(float2x4 M, float4   v) { return mul(M, v); }
float3   _ww_mul(float4   v, float3x4 M) { return mul(M, v); }
float3   _ww_mul(float3x4 M, float4   v) { return mul(M, v); }
float    _ww_mul(float a, float b)   { return a * b; }
float2   _ww_mul(float a, float2 b)  { return a * b; }
float3   _ww_mul(float a, float3 b)  { return a * b; }
float4   _ww_mul(float a, float4 b)  { return a * b; }
float2   _ww_mul(float2 a, float b)  { return a * b; }
float3   _ww_mul(float3 a, float b)  { return a * b; }
float4   _ww_mul(float4 a, float b)  { return a * b; }
float    _ww_mul(float2 a, float2 b) { return dot(a, b); }
float    _ww_mul(float3 a, float3 b) { return dot(a, b); }
float    _ww_mul(float4 a, float4 b) { return dot(a, b); }
#define mul _ww_mul

// Texture aliases such as `g_NormalMapSampler` are emitted as preprocessor defines that point at
// the concrete `g_TextureN` binding. Macro arguments used with `##` are not expanded before token
// pasting, so build the sampler-state identifier through a second macro layer: first expand the WE
// alias to `g_TextureN`, then append Hanabi's `_ww_sampler` suffix.
#define _ww_sampler_name_expanded(t) t##_ww_sampler
#define _ww_sampler_name(t) _ww_sampler_name_expanded(t)
#define texSample2D(t, uv)         ((t).Sample(_ww_sampler_name(t), (uv)))
#define texSample2DLod(t, uv, lod) ((t).SampleLevel(_ww_sampler_name(t), (uv), (lod)))
#define texSample2DCompare(t, uv, ref) ((t).SampleCmpLevelZero(_ww_sampler_name(t), (uv), (ref)))
#define texture(t, uv)             texSample2D(t, uv)
#define textureLod(t, uv, lod)     texSample2DLod(t, uv, lod)

)";

    if (stage == ShaderType::VERTEX) {
        out += "static float4 gl_Position;\n";
    } else if (stage == ShaderType::FRAGMENT) {
        out += "static float4 gl_FragCoord;\n";
        out += "static float4 glOutColor;\n";
        out += "#define gl_FragColor glOutColor\n";
    }
    out += "#define main shader_main\n";

    for (const auto& combo : combos) {
        std::string name(combo.first);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (combo.second.empty()) {
            LOG_ERROR("combo '%s' can't be empty", name.c_str());
            continue;
        }
        out += "#define " + name + " " + combo.second + "\n";
    }
    // This marker intentionally survives DXC's preprocessor. After combo/#if expansion has made
    // the live WE declarations visible, the structured adapter replaces the marker with generated
    // HLSL declarations (stage globals, cbuffer, and Texture/SamplerState pairs) that must appear
    // before the renamed user shader_main().
    out += std::string(SHADER_PLACEHOLD) + "\n";
    out += "\n";
    return out;
}

inline bool UserDefinesMod(std::string_view src) {
    static constexpr std::string_view needles[] = {
        "\nfloat mod(", "\nfloat2 mod(", "\nfloat3 mod(", "\nfloat4 mod(",
        "\nvec2 mod(",  "\nvec3 mod(",   "\nvec4 mod(",
    };
    for (auto needle : needles) {
        if (src.find(needle) != std::string_view::npos) return true;
        if (src.size() >= needle.size() - 1 && src.substr(0, needle.size() - 1) == needle.substr(1)) {
            return true;
        }
    }
    return false;
}

inline std::string CommentOutRequireDirectives(const std::string& src);
inline std::string SanitizeBrokenPreprocessorDirectives(const std::string& src,
                                                        ShaderType         type);
inline const char* DxcDebugNameForStage(ShaderType stage);
inline const char* DxcStageLogName(ShaderType stage);

inline std::string PreprocessDxcWeSource(const std::string& src, ShaderType stage,
                                         const Combos& combos,
                                         WPPreprocessorInfo& process_info) {
    std::string source = SanitizeBrokenPreprocessorDirectives(src, stage);
    source             = CommentOutRequireDirectives(source);
    source             = UndefBeforeUserMacroDefines(source, "M_PI_2");
    source             = NormalizePackedAudioSpectrumAccessImpl(source);

    std::string with_prologue;
    if (UserDefinesMod(source)) with_prologue += "#define WW_USER_MOD 1\n";
    with_prologue += BuildDxcWePrologue(combos, stage);
    with_prologue += "#line 1 \"wallpaper-we-source\"\n";
    with_prologue += source;

    vulkan::ShaderCompOpt opt;
    opt.target_env = vulkan::ShaderTargetEnv::VULKAN_1_1;

    std::string preprocessed;
    if (! vulkan::PreprocessShaderSourceWithDxc(
            with_prologue, opt, preprocessed, DxcDebugNameForStage(stage))) {
        LOG_ERROR("DXC preprocessing failed for %s shader; keeping unpreprocessed source for diagnostics",
                  DxcStageLogName(stage));
        preprocessed = std::move(with_prologue);
    }

    RecordDxcPreprocessorInfo(preprocessed, stage, process_info);
    return preprocessed;
}

inline std::string CommentOutRequireDirectives(const std::string& src) {
    std::string out;
    out.reserve(src.size());

    size_t pos { 0 };
    while (pos < src.size()) {
        const auto line_end =
            src.find('\n', pos);
        const auto line_len =
            line_end == std::string::npos ? src.size() - pos : line_end - pos;
        const auto line = std::string_view(src).substr(pos, line_len);

        const auto first_non_ws = line.find_first_not_of(" \t\r");
        if (first_non_ws != std::string_view::npos &&
            line.substr(first_non_ws).compare(0, 8, "#require") == 0) {
            out.append(line.substr(0, first_non_ws));
            out.append("//");
            out.append(line.substr(first_non_ws));
        } else {
            out.append(line);
        }

        if (line_end == std::string::npos) break;
        out.push_back('\n');
        pos = line_end + 1;
    }
    return out;
}

inline std::string RepairMissingStringQuotes(std::string_view source) {
    std::string repaired(source);
    bool        changed { false };
    bool        in_string { false };
    bool        escaped { false };
    char        string_context { '\0' };

    for (size_t i = 0; i < repaired.size(); i++) {
        const char ch = repaired[i];

        if (! in_string) {
            if (ch == '"') {
                in_string      = true;
                escaped        = false;
                string_context = PrevNonWhitespace(repaired, i);
            }
            continue;
        }

        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            in_string = false;
            continue;
        }

        // Wallpaper Engine workshop shaders occasionally omit the closing quote
        // in inline metadata comments. Only attempt a conservative repair after
        // strict parsing fails, and only for string values that appear to run
        // into the next object key or the end of the object.
        if (string_context != ':') continue;

        if (ch == ',' && LooksLikeObjectKeyAfterComma(repaired, i + 1)) {
            repaired.insert(i, 1, '"');
            changed   = true;
            in_string = false;
            i++;
            continue;
        }

        if ((ch == '}' || ch == ']') && LooksLikeContainerClose(repaired, i)) {
            repaired.insert(i, 1, '"');
            changed   = true;
            in_string = false;
            i++;
        }
    }

    if (! changed) return {};
    return repaired;
}

inline bool CanStartMetadataJsonNumber(std::string_view source, size_t pos) {
    while (pos > 0) {
        const char ch = source[--pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        return ch == '[' || ch == '{' || ch == ':' || ch == ',';
    }
    return true;
}

inline std::string NormalizeMetadataJsonNumbers(std::string_view source) {
    std::string normalized;
    normalized.reserve(source.size());
    bool in_string { false };
    bool escaped { false };
    bool changed { false };

    for (size_t i = 0; i < source.size();) {
        const char ch = source[i];
        if (in_string) {
            normalized.push_back(ch);
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            i++;
            continue;
        }

        if (ch == '"') {
            in_string = true;
            normalized.push_back(ch);
            i++;
            continue;
        }

        if ((ch == '-' || (ch >= '0' && ch <= '9')) &&
            CanStartMetadataJsonNumber(source, i)) {
            if (ch == '-') {
                normalized.push_back(ch);
                i++;
                if (i >= source.size() || source[i] < '0' || source[i] > '9') continue;
            }
            while (i + 1 < source.size() && source[i] == '0' &&
                   source[i + 1] >= '0' && source[i + 1] <= '9') {
                changed = true;
                i++;
            }
        }

        normalized.push_back(source[i++]);
    }

    return changed ? normalized : std::string {};
}

inline bool TryParseShaderMetadataJson(std::string_view line, const char* kind,
                                       nlohmann::json& result) {
    const size_t json_start = line.find_first_of('{');
    if (json_start == std::string::npos) return false;

    const auto json_source = line.substr(json_start);
    result                 = nlohmann::json::parse(json_source, nullptr, false);
    if (! result.is_discarded()) return true;

    const auto normalized_numbers = NormalizeMetadataJsonNumbers(json_source);
    if (! normalized_numbers.empty()) {
        result = nlohmann::json::parse(normalized_numbers, nullptr, false);
        if (! result.is_discarded()) {
            LOG_INFO("ParseWPShader: normalized malformed numeric %s metadata: %s",
                     kind,
                     TruncateMetadataSnippet(json_source).c_str());
            return true;
        }
    }

    const auto repaired = RepairMissingStringQuotes(json_source);
    if (! repaired.empty()) {
        result = nlohmann::json::parse(repaired, nullptr, false);
        if (! result.is_discarded()) {
            LOG_INFO("ParseWPShader: applied quote-repair to malformed %s metadata: %s",
                     kind,
                     TruncateMetadataSnippet(json_source).c_str());
            return true;
        }
    }

    LOG_INFO("ParseWPShader: skipped malformed %s metadata after quote-repair failed: %s",
             kind,
             TruncateMetadataSnippet(json_source).c_str());
    return false;
}

inline bool TextureSlotCanEnableCombo(i32 index, idx texcount,
                                      const std::vector<WPShaderTexInfo>& texinfos) {
    if (index < 0) return false;

    const auto texture_index = static_cast<usize>(index);
    // Texture-driven combos guard shader branches that sample g_TextureN. Only an authored material
    // texture should enable those branches: shader metadata defaults are editor fallbacks, not proof
    // that the project opted into an optional mask/input. Treating a default such as `util/white` as
    // bound enables Pulse's MASK branch for 2120087071 even though the material has no mask slot,
    // which changes the alpha-pulse path that makes the wallpaper rotate between images.
    return index < texcount && texture_index < texinfos.size() && texinfos[texture_index].enabled;
}

inline void ParseWPShader(const std::string& src, WPShaderInfo* pWPShaderInfo,
                          const std::vector<WPShaderTexInfo>& texinfos) {
    auto& combos       = pWPShaderInfo->combos;
    auto& wpAliasDict  = pWPShaderInfo->alias;
    auto& shadervalues = pWPShaderInfo->svs;
    auto& defTexs      = pWPShaderInfo->defTexs;
    auto& textureMaterials = pWPShaderInfo->textureMaterials;
    auto& positionUniforms = pWPShaderInfo->positionUniforms;
    idx   texcount     = std::ssize(texinfos);

    // pos start of line
    std::string::size_type pos = 0, lineEnd = std::string::npos;
    while ((lineEnd = src.find_first_of(('\n'), pos)), true) {
        const auto clineEnd = lineEnd;
        const auto line     = src.substr(pos, lineEnd - pos);

        /*
        if(line.find("attribute ") != std::string::npos || line.find("in ") != std::string::npos) {
            update_pos = true;
        }
        */
        if (line.find("// [COMBO]") != std::string::npos) {
            nlohmann::json combo_json;
            if (TryParseShaderMetadataJson(line, "combo", combo_json)) {
                if (combo_json.contains("combo")) {
                    std::string name;
                    int32_t     value = 0;
                    GET_JSON_NAME_VALUE(combo_json, "combo", name);
                    GET_JSON_NAME_VALUE(combo_json, "default", value);
                    combos[name] = std::to_string(value);
                }
            }
        } else if (line.find("uniform ") != std::string::npos) {
            if (line.find("// {") != std::string::npos) {
                nlohmann::json sv_json;
                if (TryParseShaderMetadataJson(line, "uniform", sv_json)) {
                    std::vector<std::string> defines =
                        utils::SpliteString(line.substr(0, line.find_first_of(';')), ' ');

                    std::string material;
                    GET_JSON_NAME_VALUE_NOWARN(sv_json, "material", material);
                    if (! material.empty()) wpAliasDict[material] = defines.back();

                    ShaderValue sv;
                    std::string name  = defines.back();
                    bool        istex = name.compare(0, 9, "g_Texture") == 0;
                    bool position = false;
                    GET_JSON_NAME_VALUE_NOWARN(sv_json, "position", position);
                    if (position) positionUniforms.insert(name);
                    if (istex) {
                        wpscene::WPUniformTex wput;
                        wput.FromJson(sv_json);
                        if (! material.empty()) textureMaterials[name] = material;
                        i32 index { 0 };
                        STRTONUM(name.substr(9), index);
                        if (! wput.default_.empty()) defTexs.push_back({ index, wput.default_ });
                        if (! wput.combo.empty()) {
                            const bool combo_enabled =
                                TextureSlotCanEnableCombo(index, texcount, texinfos);
                            combos[wput.combo] = combo_enabled ? "1" : "0";
                            if (! combo_enabled) {
                                LOG_VERBOSE("ParseWPShader: texture combo '%s' disabled for "
                                         "g_Texture%d because the slot has no bound texture",
                                         wput.combo.c_str(),
                                         index);
                            }
                        }
                        if (index >= 0 && index < texcount && texinfos[(usize)index].enabled) {
                            auto& compos = texinfos[(usize)index].composEnabled;

                            usize num = std::min(std::size(compos), std::size(wput.components));
                            for (usize i = 0; i < num; i++) {
                                if (compos[i]) combos[wput.components[i].combo] = "1";
                            }
                        }

                    } else {
                        if (sv_json.contains("default")) {
                            auto        value = sv_json.at("default");
                            ShaderValue sv;
                            name = defines.back();
                            if (value.is_string()) {
                                std::vector<float> v;
                                GET_JSON_VALUE(value, v);
                                sv = std::span<const float>(v);
                            } else if (value.is_number()) {
                                sv.setSize(1);
                                GET_JSON_VALUE(value, sv[0]);
                            }
                            shadervalues[name] = sv;
                        }
                        if (sv_json.contains("combo")) {
                            std::string name;
                            GET_JSON_NAME_VALUE(sv_json, "combo", name);
                            combos[name] = "1";
                        }
                    }
                    if (defines.back()[0] != 'g') {
                        LOG_VERBOSE("PreShaderSrc User shadervalue not supported: %s %s",
                                 defines.back().c_str(),
                                 sv_json.dump().c_str());
                    }
                }
            }
        }

        // end
        if (line.find("void main()") != std::string::npos || clineEnd == std::string::npos) {
            break;
        }
        pos = lineEnd + 1;
    }
}

inline usize FindIncludeInsertPos(const std::string& src, usize startPos) {
    /* rule:
    after attribute/varying/uniform/struct
    befor any func
    not in {}
    not in #if #endif
    */
    const auto mainPos = src.find("void main(", startPos);
    if (mainPos == std::string::npos) return 0;
    if (src.find("void main(", mainPos + 1) != std::string::npos) return 0;

    usize candidate_pos { 0 };
    usize pos { startPos };
    int   brace_depth { 0 };
    int   if_depth { 0 };
    bool  in_block_comment { false };
    bool  pending_struct_close { false };

    while (pos < mainPos) {
        const auto raw_line_end = src.find('\n', pos);
        const auto line_end =
            raw_line_end == std::string::npos || raw_line_end > mainPos ? mainPos : raw_line_end;
        const auto next_pos = raw_line_end == std::string::npos || raw_line_end > mainPos
                                ? line_end
                                : raw_line_end + 1;
        const auto line = std::string_view(src).substr(pos, line_end - pos);
        const auto trimmed = TrimLeadingHorizontalWhitespace(line);
        const auto brace_depth_before = brace_depth;

        const bool starts_if    = trimmed.rfind("#if", 0) == 0;
        const bool starts_endif = trimmed.rfind("#endif", 0) == 0;
        const bool top_level    = brace_depth_before == 0 && if_depth == 0;
        const bool starts_decl =
            top_level &&
            (StartsWithToken(trimmed, "attribute") || StartsWithToken(trimmed, "varying") ||
             StartsWithToken(trimmed, "uniform"));
        const bool starts_struct = top_level && StartsWithToken(trimmed, "struct");

        UpdateBraceDepth(line, brace_depth, in_block_comment);

        if (starts_decl) {
            candidate_pos = next_pos;
        }

        if (starts_struct) {
            if (line.find('{') != std::string_view::npos && brace_depth > brace_depth_before) {
                pending_struct_close = true;
            } else {
                candidate_pos = next_pos;
            }
        } else if (pending_struct_close && brace_depth_before > 0 && brace_depth == 0) {
            candidate_pos         = next_pos;
            pending_struct_close = false;
        }

        if (starts_if) {
            if_depth++;
        } else if (starts_endif && if_depth > 0) {
            if_depth--;
        }

        pos = next_pos;
    }

    return candidate_pos;
}

inline bool TextureSlotNeedsScreenSpaceYFlip(uint slot,
                                             std::span<const WPShaderTexInfo> texinfos) {
    return slot < texinfos.size() && texinfos[slot].enabled &&
        texinfos[slot].screenSpaceSampleYFlip;
}

inline bool TryParseTextureUniformSlot(std::string_view source, size_t name_pos, uint& slot) {
    if (source.substr(name_pos, 9) != "g_Texture") return false;

    const auto slot_begin = name_pos + 9;
    auto       slot_end   = slot_begin;
    while (slot_end < source.size() &&
           std::isdigit(static_cast<unsigned char>(source[slot_end]))) {
        slot_end++;
    }
    if (slot_end == slot_begin) return false;

    auto [ptr, ec] =
        std::from_chars(source.data() + slot_begin, source.data() + slot_end, slot);
    return ec == std::errc() && ptr == source.data() + slot_end;
}

inline size_t FindTopLevelComma(std::string_view source, size_t begin, size_t end) {
    int  paren_depth { 0 };
    int  bracket_depth { 0 };
    bool in_block_comment { false };
    bool in_string { false };
    bool escaped { false };
    char quote { '\0' };

    for (size_t pos = begin; pos < end; pos++) {
        const char ch   = source[pos];
        const char next = pos + 1 < end ? source[pos + 1] : '\0';

        if (in_block_comment) {
            if (ch == '*' && next == '/') {
                in_block_comment = false;
                pos++;
            }
            continue;
        }
        if (in_string) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == quote) in_string = false;
            continue;
        }
        if (ch == '/' && next == '/') {
            const auto line_end = source.find('\n', pos);
            if (line_end == std::string_view::npos || line_end >= end) break;
            pos = line_end;
            continue;
        }
        if (ch == '/' && next == '*') {
            in_block_comment = true;
            pos++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_string = true;
            quote     = ch;
            continue;
        }
        if (ch == '(') paren_depth++;
        else if (ch == ')' && paren_depth > 0) paren_depth--;
        else if (ch == '[') bracket_depth++;
        else if (ch == ']' && bracket_depth > 0) bracket_depth--;
        else if (ch == ',' && paren_depth == 0 && bracket_depth == 0) return pos;
    }
    return std::string_view::npos;
}

inline std::string ApplyScreenSpaceTextureSampleYFlipForFunction(
    const std::string& source,
    std::string_view   function_name,
    bool               has_lod_argument,
    std::span<const WPShaderTexInfo> texinfos) {
    std::string result;
    result.reserve(source.size());

    size_t cursor { 0 };
    size_t search_pos { 0 };
    while (true) {
        const auto function_pos = source.find(function_name, search_pos);
        if (function_pos == std::string::npos) break;

        const bool left_boundary =
            function_pos == 0 || !IsIdentifierContinue(source[function_pos - 1]);
        const auto after_name = function_pos + function_name.size();
        const bool right_boundary =
            after_name >= source.size() || !IsIdentifierContinue(source[after_name]);
        if (!left_boundary || !right_boundary) {
            search_pos = after_name;
            continue;
        }

        const auto open_pos = SkipWhitespace(source, after_name);
        if (open_pos >= source.size() || source[open_pos] != '(') {
            search_pos = after_name;
            continue;
        }
        const auto close_after_pos = SkipBalanced(source, open_pos, '(', ')');
        if (close_after_pos == std::string::npos || close_after_pos > source.size() ||
            close_after_pos <= open_pos + 1) {
            search_pos = after_name;
            continue;
        }
        const auto close_pos = close_after_pos - 1;

        const auto first_arg_begin = SkipWhitespace(source, open_pos + 1);
        uint       slot { 0 };
        if (!TryParseTextureUniformSlot(source, first_arg_begin, slot) ||
            !TextureSlotNeedsScreenSpaceYFlip(slot, texinfos)) {
            search_pos = close_after_pos;
            continue;
        }

        const auto first_arg_end = SkipIdentifier(source, first_arg_begin);
        auto       comma_pos     = SkipWhitespace(source, first_arg_end);
        if (comma_pos >= close_pos || source[comma_pos] != ',') {
            search_pos = close_after_pos;
            continue;
        }

        const auto second_arg_begin = comma_pos + 1;
        const auto second_arg_end =
            has_lod_argument ? FindTopLevelComma(source, second_arg_begin, close_pos) : close_pos;
        if (second_arg_end == std::string_view::npos || second_arg_end <= second_arg_begin) {
            search_pos = close_after_pos;
            continue;
        }

        auto value_begin = SkipWhitespace(source, second_arg_begin);
        auto value_end   = second_arg_end;
        while (value_end > value_begin &&
               std::isspace(static_cast<unsigned char>(source[value_end - 1]))) {
            value_end--;
        }
        if (value_begin >= value_end) {
            search_pos = close_after_pos;
            continue;
        }

        // This is a slot-aware shader preparation transform, not a shader-name workaround. Runtime
        // render-target metadata decides which g_TextureN bindings need screen-space Y correction,
        // while the producer pass and all unrelated 2D texture samples remain unchanged.
        result.append(source, cursor, value_begin - cursor);
        result.append("wpFlipSampleY(");
        result.append(source, value_begin, value_end - value_begin);
        result.push_back(')');
        cursor = value_end;
        search_pos = close_after_pos;
    }

    result.append(source, cursor, std::string::npos);
    return result;
}

inline std::string ApplyScreenSpaceTextureSampleYFlip(
    const std::string& source,
    std::span<const WPShaderTexInfo> texinfos) {
    auto result = ApplyScreenSpaceTextureSampleYFlipForFunction(
        source, "texSample2DLod", true, texinfos);
    result = ApplyScreenSpaceTextureSampleYFlipForFunction(
        result, "texSample2D", false, texinfos);
    return result;
}

inline std::string SanitizeBrokenPreprocessorDirectives(const std::string& src,
                                                        ShaderType         type) {
    std::string out;
    out.reserve(src.size());

    struct IfFrame {
        std::string::size_type pos { 0 };
        bool                   runtime_condition { false };
    };

    const auto trim_view = [](std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) return std::string_view {};
        const auto last = text.find_last_not_of(" \t\r");
        return text.substr(first, last - first + 1);
    };
    const auto runtime_if_condition = [&](std::string_view directive,
                                          std::string_view keyword) -> std::optional<std::string> {
        if (directive.size() <= keyword.size()) return std::nullopt;

        auto condition = trim_view(directive.substr(keyword.size()));
        if (condition.empty()) return std::nullopt;

        // Some stock WE effect shaders use texture-resolution uniforms directly inside `#if`
        // expressions, for example `#if g_Texture0Resolution.y <= 640`. DXC only accepts integer
        // macro expressions there, so preserve the authored branch by lowering this narrow pattern
        // into an ordinary runtime `if (...)` block instead of dropping the pass entirely.
        if (condition.find("g_Texture") == std::string_view::npos ||
            condition.find("Resolution.") == std::string_view::npos) {
            return std::nullopt;
        }

        return std::string(condition);
    };
    const auto repair_runtime_condition_line = [&](std::string_view line) {
        const auto trimmed = trim_view(line);
        if (trimmed.empty() || trimmed.starts_with("//")) return std::string(line);
        if (trimmed.back() == ';' || trimmed.back() == '{' || trimmed.back() == '}') {
            return std::string(line);
        }
        // The stock AA effect carries branch-local assignments such as `v_TexCoord.w = 0`
        // without a trailing semicolon inside the compile-time `#if` block. Once that block is
        // lowered into a runtime `if`, HLSL needs the ordinary statement terminator.
        if (trimmed.find('=') != std::string_view::npos) {
            std::string repaired(line);
            repaired.push_back(';');
            return repaired;
        }
        return std::string(line);
    };

    std::vector<IfFrame> if_stack;
    std::string::size_type              pos { 0 };
    usize                               removed_endifs { 0 };

    while (pos < src.size()) {
        auto line_end = src.find('\n', pos);
        auto line_len =
            line_end == std::string::npos ? src.size() - pos : line_end - pos;
        auto line = src.substr(pos, line_len);

        auto first_non_ws = line.find_first_not_of(" \t\r");
        bool handled      = false;
        if (first_non_ws != std::string::npos && line[first_non_ws] == '#') {
            const auto directive  = std::string_view(line).substr(first_non_ws);
            const auto line_prefix = std::string_view(line).substr(0, first_non_ws);
            if (directive.rfind("#if ", 0) == 0 || directive == "#if") {
                if (const auto condition = runtime_if_condition(directive, "#if");
                    condition.has_value()) {
                    out.append(line_prefix);
                    out.append("if (");
                    out.append(*condition);
                    out.append(") {");
                    handled = true;
                    if_stack.push_back(IfFrame { .pos = pos, .runtime_condition = true });
                } else {
                    if_stack.push_back(IfFrame { .pos = pos, .runtime_condition = false });
                }
            } else if (directive.rfind("#ifdef", 0) == 0 ||
                       directive.rfind("#ifndef", 0) == 0) {
                if_stack.push_back(IfFrame { .pos = pos, .runtime_condition = false });
            } else if (directive.rfind("#elif ", 0) == 0 || directive == "#elif") {
                if (! if_stack.empty() && if_stack.back().runtime_condition) {
                    const auto condition =
                        runtime_if_condition(directive, "#elif").value_or(
                            std::string(trim_view(directive.substr(5))));
                    out.append(line_prefix);
                    out.append("} else if (");
                    out.append(condition);
                    out.append(") {");
                    handled = true;
                }
            } else if (directive.rfind("#else", 0) == 0) {
                if (! if_stack.empty() && if_stack.back().runtime_condition) {
                    out.append(line_prefix);
                    out.append("} else {");
                    handled = true;
                }
            } else if (directive.rfind("#endif", 0) == 0) {
                if (if_stack.empty()) {
                    out.append(line.substr(0, first_non_ws));
                    out.append("// stripped unmatched #endif");
                    handled = true;
                    removed_endifs++;
                } else if (if_stack.back().runtime_condition) {
                    out.append(line_prefix);
                    out.append("}");
                    handled = true;
                    if_stack.pop_back();
                } else {
                    if_stack.pop_back();
                }
            }
        }

        if (!handled) {
            if (! if_stack.empty() && if_stack.back().runtime_condition &&
                !(first_non_ws != std::string::npos && line[first_non_ws] == '#')) {
                out.append(repair_runtime_condition_line(line));
            } else {
                out.append(line);
            }
        }
        if (line_end == std::string::npos) break;
        out.push_back('\n');
        pos = line_end + 1;
    }

    if (removed_endifs > 0) {
        LOG_VERBOSE("SanitizeBrokenPreprocessorDirectives stripped %zu unmatched #endif line(s) "
                    "from %s shader",
                    removed_endifs,
                    type == ShaderType::VERTEX ? "vertex" : "fragment");
    }
    return out;
}

inline std::string GenSha1(std::span<const WPShaderUnit> units) {
    std::string shas;
    for (auto& unit : units) {
        shas += utils::genSha1(unit.src);
    }
    return utils::genSha1(shas);
}
inline std::string GetCachePath(std::string_view scene_id, std::string_view filename) {
    return std::string("/cache/") + std::string(scene_id) + "/" SHADER_DIR "/" +
           std::string(filename) + "." SHADER_SUFFIX;
}

inline std::string GetPreShaderCachePath(std::string_view filename) {
    return std::string("/cache/") + SHADER_META_DIR + "/" + std::string(filename) + "." +
           SHADER_META_SUFFIX;
}

inline std::string GetPreparedShaderCachePath(std::string_view scene_id, std::string_view filename) {
    return std::string("/cache/") + std::string(scene_id) + "/" SHADER_SRC_DIR "/" +
           std::string(filename) + "." SHADER_SRC_SUFFIX;
}

inline bool LoadShaderFromFile(std::vector<ShaderCode>& codes, fs::IBinaryStream& file) {
    codes.clear();
    i32 ver = ReadSPVVesion(file);

    usize count = file.ReadUint32();
    assert(count <= 16 && count >= 0);
    if (count > 16) return false;

    codes.resize(count);
    for (usize i = 0; i < count; i++) {
        auto& c = codes[i];

        u32 size = file.ReadUint32();
        assert(size % 4 == 0);
        if (size % 4 != 0) return false;

        c.resize(size / 4);
        file.Read((char*)c.data(), size);
    }
    return true;
}

inline void SaveShaderToFile(std::span<const ShaderCode> codes, fs::IBinaryStreamW& file) {
    char nop[256] { '\0' };

    WriteSPVVesion(file, 1);
    file.WriteUint32((u32)codes.size());
    for (const auto& c : codes) {
        u32 size = (u32)c.size() * 4;
        file.WriteUint32(size);
        file.Write((const char*)c.data(), size);
    }
    file.Write(nop, sizeof(nop));
}

inline std::string GenPreparedShaderSha1(std::span<const WPShaderUnit> units,
                                         const Combos& combos,
                                         std::span<const WPShaderTexInfo> texinfos) {
    std::ostringstream out;
    out << kPreparedShaderPipelineKey;
    for (const auto& unit : units) {
        out << static_cast<int>(unit.stage) << '\n';
        out << utils::genSha1(unit.src) << '\n';
    }
    for (const auto& [name, value] : combos) {
        out << name << '=' << value << '\n';
    }
    for (const auto& texinfo : texinfos) {
        out << static_cast<int>(texinfo.enabled)
            << static_cast<int>(texinfo.screenSpaceSampleYFlip) << '\n';
    }
    const auto data = out.str();
    return utils::genSha1(std::span<const char>(data.data(), data.size()));
}

inline std::string GenPreShaderSha1(std::string_view expanded_src,
                                    std::span<const WPShaderTexInfo> texinfos) {
    std::ostringstream out;
    out << "pre-shader-v5-quad-position-metadata\n";
    out << utils::genSha1(expanded_src) << '\n';
    for (const auto& texinfo : texinfos) {
        out << static_cast<int>(texinfo.enabled);
        out << static_cast<int>(texinfo.screenSpaceSampleYFlip);
        for (const auto component_enabled : texinfo.composEnabled) {
            out << static_cast<int>(component_enabled);
        }
        out << '\n';
    }

    const auto data = out.str();
    return utils::genSha1(std::span<const char>(data.data(), data.size()));
}

inline void WriteString(fs::IBinaryStreamW& file, std::string_view value) {
    file.WriteUint32(static_cast<u32>(value.size()));
    if (! value.empty()) file.Write(value.data(), value.size());
}

inline bool ReadString(fs::IBinaryStream& file, std::string& value) {
    const auto size = file.ReadUint32();
    value.resize(size);
    if (size > 0) file.Read(value.data(), size);
    return true;
}

inline bool LoadStringMap(Map<std::string, std::string>& map, fs::IBinaryStream& file) {
    map.clear();

    const auto count = file.ReadUint32();
    for (u32 i = 0; i < count; i++) {
        std::string key;
        std::string value;
        if (! ReadString(file, key)) return false;
        if (! ReadString(file, value)) return false;

        map.emplace(std::move(key), std::move(value));
    }
    return true;
}

inline void SaveStringMap(const Map<std::string, std::string>& map, fs::IBinaryStreamW& file) {
    file.WriteUint32(static_cast<u32>(map.size()));
    for (const auto& [key, value] : map) {
        WriteString(file, key);
        WriteString(file, value);
    }
}

inline bool LoadStringSet(Set<std::string>& set, fs::IBinaryStream& file) {
    set.clear();
    const auto count = file.ReadUint32();
    for (u32 i = 0; i < count; i++) {
        std::string value;
        if (! ReadString(file, value)) return false;
        set.insert(std::move(value));
    }
    return true;
}

inline void SaveStringSet(const Set<std::string>& set, fs::IBinaryStreamW& file) {
    file.WriteUint32(static_cast<u32>(set.size()));
    for (const auto& value : set) WriteString(file, value);
}

inline bool LoadShaderValue(ShaderValue& value, fs::IBinaryStream& file) {
    const auto size = file.ReadUint32();
    std::vector<float> values(size);
    for (u32 i = 0; i < size; i++) {
        values[i] = file.ReadFloat();
    }
    value = ShaderValue(values);
    return true;
}

inline void SaveShaderValue(const ShaderValue& value, fs::IBinaryStreamW& file) {
    file.WriteUint32(static_cast<u32>(value.size()));
    for (size_t i = 0; i < value.size(); i++) {
        const float component = value[i];
        file.Write(&component, sizeof(component));
    }
}

inline bool LoadShaderValueMap(ShaderValueMap& map, fs::IBinaryStream& file) {
    map.clear();

    const auto count = file.ReadUint32();
    for (u32 i = 0; i < count; i++) {
        std::string key;
        ShaderValue value;
        if (! ReadString(file, key)) return false;
        if (! LoadShaderValue(value, file)) return false;
        map.emplace(std::move(key), std::move(value));
    }
    return true;
}

inline void SaveShaderValueMap(const ShaderValueMap& map, fs::IBinaryStreamW& file) {
    file.WriteUint32(static_cast<u32>(map.size()));
    for (const auto& [key, value] : map) {
        WriteString(file, key);
        SaveShaderValue(value, file);
    }
}

inline bool LoadDefaultTexs(WPDefaultTexs& def_texs, fs::IBinaryStream& file) {
    def_texs.clear();

    const auto count = file.ReadUint32();
    def_texs.reserve(count);
    for (u32 i = 0; i < count; i++) {
        const auto slot = file.ReadInt32();
        std::string default_tex;
        if (! ReadString(file, default_tex)) return false;
        def_texs.emplace_back(slot, std::move(default_tex));
    }
    return true;
}

inline void SaveDefaultTexs(const WPDefaultTexs& def_texs, fs::IBinaryStreamW& file) {
    file.WriteUint32(static_cast<u32>(def_texs.size()));
    for (const auto& [slot, default_tex] : def_texs) {
        file.WriteInt32(slot);
        WriteString(file, default_tex);
    }
}

inline bool LoadPreShaderInfo(WPShaderInfo& shader_info, fs::IBinaryStream& file) {
    const auto version = ReadVersion("WSHM", file);
    if (version != 3) return false;

    shader_info = {};
    if (! LoadStringMap(shader_info.combos, file)) return false;
    if (! LoadStringMap(shader_info.alias, file)) return false;
    if (! LoadShaderValueMap(shader_info.svs, file)) return false;
    if (! LoadDefaultTexs(shader_info.defTexs, file)) return false;
    if (! LoadStringMap(shader_info.textureMaterials, file)) return false;
    if (! LoadStringSet(shader_info.positionUniforms, file)) return false;
    return true;
}

inline void SavePreShaderInfo(const WPShaderInfo& shader_info, fs::IBinaryStreamW& file) {
    WriteVersion("WSHM", file, 3);
    SaveStringMap(shader_info.combos, file);
    SaveStringMap(shader_info.alias, file);
    SaveShaderValueMap(shader_info.svs, file);
    SaveDefaultTexs(shader_info.defTexs, file);
    SaveStringMap(shader_info.textureMaterials, file);
    SaveStringSet(shader_info.positionUniforms, file);
}

inline void MergeShaderInfo(WPShaderInfo& into, const WPShaderInfo& from) {
    for (const auto& [key, value] : from.combos) {
        into.combos[key] = value;
    }
    for (const auto& [key, value] : from.alias) {
        into.alias[key] = value;
    }
    for (const auto& [key, value] : from.svs) {
        into.svs[key] = value;
    }
    into.defTexs.insert(into.defTexs.end(), from.defTexs.begin(), from.defTexs.end());
    for (const auto& [key, value] : from.textureMaterials) {
        into.textureMaterials[key] = value;
    }
    into.positionUniforms.insert(from.positionUniforms.begin(), from.positionUniforms.end());
}

struct ExpandedShaderSource {
    std::string expanded_src;
    std::string src_without_includes;
    std::string include_src;
};

inline ExpandedShaderSource ExpandShaderSource(fs::VFS& vfs, const std::string& src) {
    ExpandedShaderSource   result { .expanded_src = src, .src_without_includes = src, .include_src = {} };
    std::string::size_type pos = 0;
    while (pos = src.find("#include", pos), pos != std::string::npos) {
        auto begin = pos;
        pos        = src.find_first_of('\n', pos);
        result.src_without_includes.replace(begin, pos - begin, pos - begin, ' ');
        result.include_src.append(src.substr(begin, pos - begin) + "\n");
    }

    result.include_src = LoadGlslInclude(vfs, result.include_src);
    result.expanded_src = result.src_without_includes;
    result.expanded_src.insert(FindIncludeInsertPos(result.expanded_src, 0), result.include_src);
    return result;
}

inline bool LoadPreparedShaderUnits(std::span<WPShaderUnit> units, fs::IBinaryStream& file) {
    const auto version = ReadVersion("WSRC", file);
    if (version != kPreparedShaderSourceVersion) return false;

    const auto count = file.ReadUint32();
    if (count != units.size()) return false;

    for (usize i = 0; i < units.size(); i++) {
        const auto stage = file.ReadInt32();
        if (stage != static_cast<int32_t>(units[i].stage)) return false;

        const auto size = file.ReadUint32();
        std::string src(size, '\0');
        if (size > 0) file.Read(src.data(), size);
        units[i].src = std::move(src);

        auto& info = units[i].preprocess_info;
        info       = {};

        if (! LoadStringMap(info.input, file)) return false;
        if (! LoadStringMap(info.output, file)) return false;
        if (! LoadStringMap(info.uniforms, file)) return false;

        const auto active_count = file.ReadUint32();
        for (u32 j = 0; j < active_count; j++) {
            info.active_tex_slots.insert(file.ReadUint32());
        }
    }
    return true;
}

inline void SavePreparedShaderUnits(std::span<const WPShaderUnit> units, fs::IBinaryStreamW& file) {
    WriteVersion("WSRC", file, kPreparedShaderSourceVersion);
    file.WriteUint32(static_cast<u32>(units.size()));
    for (const auto& unit : units) {
        file.WriteInt32(static_cast<int32_t>(unit.stage));
        file.WriteUint32(static_cast<u32>(unit.src.size()));
        if (! unit.src.empty()) file.Write(unit.src.data(), unit.src.size());

        SaveStringMap(unit.preprocess_info.input, file);
        SaveStringMap(unit.preprocess_info.output, file);
        SaveStringMap(unit.preprocess_info.uniforms, file);

        file.WriteUint32(static_cast<u32>(unit.preprocess_info.active_tex_slots.size()));
        for (const auto slot : unit.preprocess_info.active_tex_slots) {
            file.WriteUint32(slot);
        }
    }
}

inline void AddUniqueIODecl(std::vector<IODecl>& decls, Set<std::string>& seen,
                            const IODecl& decl) {
    if (decl.name == "gl_Position" || decl.name == "_ww_sv_position") return;
    if (! seen.insert(decl.name).second) return;
    decls.push_back(decl);
}

inline void AddUniqueIODeclFromLine(std::vector<IODecl>& decls, Set<std::string>& seen,
                                    const std::string& line) {
    if (auto decl = ParseIODecl(line); decl.has_value()) {
        AddUniqueIODecl(decls, seen, *decl);
    }
}

inline std::string FinalprocessDxcWeSource(const WPShaderUnit& unit,
                                           const WPPreprocessorInfo* pre,
                                           const WPPreprocessorInfo* next,
                                           const Map<std::string, std::string>& uniforms_union) {
    auto [io_decls, without_io]      = ScanAndStripIO(unit.src);
    auto [samplers, without_samplers] = ScanAndStripSamplers(without_io);
    auto body                        = StripUniforms(without_samplers);
    body                             = StripDxcLineDirectives(body);
    size_t top_level_const_updates { 0 };
    body = MakeTopLevelConstGlobalsStaticForDxc(body, top_level_const_updates);
    if (top_level_const_updates > 0) {
        LOG_INFO("DXC WE finalization made %zu top-level const globals static for %s shader",
                 top_level_const_updates,
                 DxcStageLogName(unit.stage));
    }

    std::vector<IODecl> attrs;
    std::vector<IODecl> varyings;
    Set<std::string>    seen_attrs;
    Set<std::string>    seen_varyings;

    // Wallpaper Engine shaders are authored as stage snippets, not complete HLSL entry points.
    // The producer stage owns the cross-stage varying type when declarations disagree (for
    // example a vertex shader writes `varying vec2 v_TexCoord` while the fragment shader declares
    // `varying vec4 v_TexCoord` and only reads `.xy`). This mirrors the existing renderer
    // behavior without carrying the GLSL-era expression transform chain into the DXC path.
    if (unit.stage == ShaderType::FRAGMENT && pre != nullptr) {
        for (const auto& [name, line] : pre->output) {
            (void)name;
            AddUniqueIODeclFromLine(varyings, seen_varyings, line);
        }
    }

    for (const auto& decl : io_decls) {
        if (decl.storage == 'a') {
            AddUniqueIODecl(attrs, seen_attrs, decl);
        } else {
            AddUniqueIODecl(varyings, seen_varyings, decl);
        }
    }

    if (unit.stage != ShaderType::FRAGMENT && next != nullptr) {
        for (const auto& [name, line] : next->input) {
            (void)name;
            AddUniqueIODeclFromLine(varyings, seen_varyings, line);
        }
    }

    auto synth = SynthesizeDxcEntry(unit.stage, attrs, varyings);

    std::string generated;
    generated += synth.pre;
    if (! uniforms_union.empty()) {
        generated += "\n// === auto-generated WE uniform block ===\n";
        generated += EmitCBufferStd140(uniforms_union);
    }
    if (! samplers.empty()) {
        generated += "\n// === auto-generated WE samplers ===\n";
        generated += EmitSamplerBlock(samplers);
    }

    // DXC has already preprocessed the WE source, so the placeholder is now just a stable splice
    // point for declarations that must precede the renamed user shader_main(). Appending the
    // wrapper after the body keeps the call target visible without forcing source-order hacks into
    // authored shaders.
    const bool inserted_generated_block = ReplaceAll(body, SHADER_PLACEHOLD, generated);
    if (! inserted_generated_block) {
        LOG_ERROR("DXC WE finalization could not find shader splice marker for %s shader",
                  DxcStageLogName(unit.stage));
    }
    return body + synth.post;
}

inline void PrepareShaderUnitsForDxc(std::span<WPShaderUnit> units,
                                     WPShaderInfo* shader_info,
                                     std::span<const WPShaderTexInfo> texinfos) {
    for (auto& unit : units) {
        unit.src = ApplyScreenSpaceTextureSampleYFlip(unit.src, texinfos);
        unit.src = PreprocessDxcWeSource(unit.src, unit.stage, shader_info->combos, unit.preprocess_info);
    }

    Map<std::string, std::string> uniforms_union;
    for (const auto& unit : units) {
        for (const auto& [name, type] : unit.preprocess_info.uniforms) {
            uniforms_union.try_emplace(name, type);
        }
    }

    for (usize i = 0; i < units.size(); i++) {
        auto&               unit      = units[i];
        WPPreprocessorInfo* pre_info  = i >= 1 ? &units[i - 1].preprocess_info : nullptr;
        WPPreprocessorInfo* post_info = i + 1 < units.size() ? &units[i + 1].preprocess_info
                                                             : nullptr;
        unit.src = FinalprocessDxcWeSource(unit, pre_info, post_info, uniforms_union);
    }
}

inline const char* DxcEntryPointForStage(ShaderType stage) {
    switch (stage) {
    case ShaderType::VERTEX: return "main_vs";
    case ShaderType::GEOMETRY: return "main_gs";
    case ShaderType::FRAGMENT: return "main_ps";
    }
    return "main";
}

inline const char* DxcDebugNameForStage(ShaderType stage) {
    switch (stage) {
    case ShaderType::VERTEX: return "wallpaper-we.vert";
    case ShaderType::GEOMETRY: return "wallpaper-we.geom";
    case ShaderType::FRAGMENT: return "wallpaper-we.frag";
    }
    return "wallpaper-we.shader";
}

inline const char* DxcStageLogName(ShaderType stage) {
    switch (stage) {
    case ShaderType::VERTEX: return "vertex";
    case ShaderType::GEOMETRY: return "geometry";
    case ShaderType::FRAGMENT: return "fragment";
    }
    return "unknown";
}

} // namespace

std::string wallpaper::test::NormalizePackedAudioSpectrumAccess(std::string_view source) {
    return NormalizePackedAudioSpectrumAccessImpl(source);
}

std::string WPShaderParser::PreShaderSrc(fs::VFS& vfs, const std::string& src,
                                         WPShaderInfo*                       pWPShaderInfo,
                                         const std::vector<WPShaderTexInfo>& texinfos) {
    auto expanded = ExpandShaderSource(vfs, src);

    if (! vfs.IsMounted("cache")) {
        ParseWPShader(expanded.include_src, pWPShaderInfo, texinfos);
        ParseWPShader(expanded.src_without_includes, pWPShaderInfo, texinfos);
        return expanded.expanded_src;
    }

    const auto cache_key  = GenPreShaderSha1(expanded.expanded_src, texinfos);
    const auto cache_path = GetPreShaderCachePath(cache_key);

    WPShaderInfo cached_info;
    if (vfs.Contains(cache_path)) {
        auto cache_file = vfs.Open(cache_path);
        if (! cache_file || ! LoadPreShaderInfo(cached_info, *cache_file)) {
            LOG_ERROR("load pre-shader metadata from '%s' failed", cache_path.c_str());
            return {};
        }
    } else {
        ParseWPShader(expanded.include_src, &cached_info, texinfos);
        ParseWPShader(expanded.src_without_includes, &cached_info, texinfos);

        if (auto cache_file = vfs.OpenW(cache_path); cache_file) {
            SavePreShaderInfo(cached_info, *cache_file);
        }
    }

    MergeShaderInfo(*pWPShaderInfo, cached_info);
    return expanded.expanded_src;
}

bool WPShaderParser::CompileToSpv(std::string_view scene_id, std::span<WPShaderUnit> units,
                                  std::vector<ShaderCode>& codes, fs::VFS& vfs,
                                  WPShaderInfo* shader_info, std::span<const WPShaderTexInfo> texs) {
    auto compile = [](std::span<WPShaderUnit> units, std::vector<ShaderCode>& codes) {
        std::vector<vulkan::ShaderCompUnit> vunits(units.size());
        for (usize i = 0; i < units.size(); i++) {
            auto&               unit     = units[i];
            auto&               vunit    = vunits[i];

            vunit.src             = unit.src;
            vunit.stage           = unit.stage;
            vunit.source_language = vulkan::ShaderSourceLanguage::HLSL;
            vunit.debug_name      = DxcDebugNameForStage(unit.stage);
            vunit.entry_point     = DxcEntryPointForStage(unit.stage);
        }

        vulkan::ShaderCompOpt opt;
        opt.target_env             = vulkan::ShaderTargetEnv::VULKAN_1_1;
        opt.auto_map_bindings      = true;
        opt.auto_map_locations     = true;

        std::vector<vulkan::Uni_ShaderSpv> spvs(units.size());

        if (! vulkan::CompileAndLinkShaderUnits(vunits, opt, spvs)) {
            return false;
        }

        codes.clear();
        for (auto& spv : spvs) {
            codes.emplace_back(std::move(spv->spirv));
        }
        return true;
    };

    bool has_cache_dir = vfs.IsMounted("cache");

    if (has_cache_dir) {
        const std::string prepared_sha1 = GenPreparedShaderSha1(units, shader_info->combos, texs);
        const std::string prepared_cache_file_path = GetPreparedShaderCachePath(scene_id, prepared_sha1);

        if (vfs.Contains(prepared_cache_file_path)) {
            auto cache_file = vfs.Open(prepared_cache_file_path);
            if (! cache_file || ! ::LoadPreparedShaderUnits(units, *cache_file)) {
                LOG_ERROR("load prepared shader from '%s' failed", prepared_cache_file_path.c_str());
                return false;
            }
        } else {
            PrepareShaderUnitsForDxc(units, shader_info, texs);
            if (auto cache_file = vfs.OpenW(prepared_cache_file_path); cache_file) {
                ::SavePreparedShaderUnits(units, *cache_file);
            }
        }

        const std::string cache_key_input =
            "spv-backend=dxc\n"
            "we-pipeline=dxc-perspective-matrix-compat-v1\n" + GenSha1(units);
        std::string sha1 = utils::genSha1(
            std::span<const char>(cache_key_input.data(), cache_key_input.size()));
        std::string cache_file_path = GetCachePath(scene_id, sha1);

        if (vfs.Contains(cache_file_path)) {
            auto cache_file = vfs.Open(cache_file_path);
            if (! cache_file || ! ::LoadShaderFromFile(codes, *cache_file)) {
                LOG_ERROR("load shader from \'%s\' failed", cache_file_path.c_str());
                return false;
            }
        } else {
            if (! compile(units, codes)) return false;
            if (auto cache_file = vfs.OpenW(cache_file_path); cache_file) {
                ::SaveShaderToFile(codes, *cache_file);
            }
        }
        return true;

    } else {
        PrepareShaderUnitsForDxc(units, shader_info, texs);
        return compile(units, codes);
    }
}
