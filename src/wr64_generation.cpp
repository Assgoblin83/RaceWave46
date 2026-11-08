#include "wr64_generation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include "json/json.hpp"
#include "wr64_sha256.hpp"

namespace wr64::generation {
namespace {

bool is_space(unsigned char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
        value == '\f' || value == '\v';
}

bool is_digit(unsigned char value) {
    return value >= '0' && value <= '9';
}

bool is_hex(unsigned char value) {
    return is_digit(value) || (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

bool is_identifier_start(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        value == '_';
}

bool is_identifier(unsigned char value) {
    return is_identifier_start(value) || is_digit(value);
}

// Matches one complete line of the form "case 0x...: goto L_...;\n".
bool is_dispatch_line(std::string_view line) {
    size_t index = 0;
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
        ++index;
    }
    auto literal = [&](std::string_view text) {
        if (line.substr(index, text.size()) != text) {
            return false;
        }
        index += text.size();
        return true;
    };
    auto digits = [&](bool (*predicate)(unsigned char)) {
        const size_t start = index;
        while (index < line.size() && predicate(static_cast<unsigned char>(line[index]))) {
            ++index;
        }
        return index > start;
    };

    if (!literal("case 0x")) {
        return false;
    }
    if (!digits(is_hex) || !literal(": goto L_") || !digits(is_hex)) {
        return false;
    }
    return line.substr(index) == ";\n";
}

std::string sort_dispatch_runs(std::string_view text) {
    std::string output;
    output.reserve(text.size());

    size_t index = 0;
    while (index < text.size()) {
        const size_t line_end = text.find('\n', index);
        if (line_end == std::string_view::npos) {
            output.append(text.substr(index));
            break;
        }
        const std::string_view line = text.substr(index, line_end - index + 1);
        if (!is_dispatch_line(line)) {
            output.append(line);
            index = line_end + 1;
            continue;
        }

        std::vector<std::string_view> run;
        size_t cursor = index;
        while (cursor < text.size()) {
            const size_t end = text.find('\n', cursor);
            if (end == std::string_view::npos) {
                break;
            }
            const std::string_view candidate = text.substr(cursor, end - cursor + 1);
            if (!is_dispatch_line(candidate)) {
                break;
            }
            run.push_back(candidate);
            cursor = end + 1;
        }
        std::sort(run.begin(), run.end());
        for (const std::string_view sorted : run) {
            output.append(sorted);
        }
        index = cursor;
    }
    return output;
}

size_t match_quoted(std::string_view text, size_t start, char quote) {
    size_t index = start + 1;
    while (index < text.size()) {
        if (text[index] == '\\') {
            index += 2;
            continue;
        }
        if (text[index] == quote) {
            return index + 1;
        }
        ++index;
    }
    return start + 1;
}

size_t match_number(std::string_view text, size_t start) {
    size_t index = start;
    if (text[index] == '0' && index + 1 < text.size() &&
        (text[index + 1] == 'x' || text[index + 1] == 'X')) {
        size_t cursor = index + 2;
        while (cursor < text.size() && is_hex(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }
        if (cursor > index + 2) {
            index = cursor;
        }
    }
    if (index == start) {
        while (index < text.size() && is_digit(static_cast<unsigned char>(text[index]))) {
            ++index;
        }
        if (index < text.size() && text[index] == '.') {
            ++index;
            while (index < text.size() && is_digit(static_cast<unsigned char>(text[index]))) {
                ++index;
            }
        }
        if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
            size_t exponent = index + 1;
            if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) {
                ++exponent;
            }
            const size_t digits = exponent;
            while (exponent < text.size() && is_digit(static_cast<unsigned char>(text[exponent]))) {
                ++exponent;
            }
            if (exponent > digits) {
                index = exponent;
            }
        }
    }
    while (index < text.size() && (text[index] == 'u' || text[index] == 'U' ||
        text[index] == 'l' || text[index] == 'L' || text[index] == 'f' || text[index] == 'F')) {
        ++index;
    }
    return index;
}

size_t match_operator(std::string_view text, size_t start) {
    static constexpr std::array<std::string_view, 18> kOperators = {
        ">>=", "<<=", "->", "++", "--", "<<", ">>", "<=", ">=",
        "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=",
    };
    for (const std::string_view candidate : kOperators) {
        if (text.substr(start, candidate.size()) == candidate) {
            return start + candidate.size();
        }
    }
    static constexpr std::array<std::string_view, 3> kMore = {"&=", "|=", "^="};
    for (const std::string_view candidate : kMore) {
        if (text.substr(start, candidate.size()) == candidate) {
            return start + candidate.size();
        }
    }
    return start + 1;
}

bool read_file(const std::filesystem::path& path, std::string& contents, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "Cannot read " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    contents = buffer.str();
    return true;
}

bool write_file(const std::filesystem::path& path, std::string_view contents, std::string& error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "Cannot write " + path.string();
        return false;
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream) {
        error = "Cannot write " + path.string();
        return false;
    }
    return true;
}

bool replace_once(std::string& text, std::string_view from, std::string_view to,
    std::string& error) {
    const size_t first = text.find(from);
    if (first == std::string::npos) {
        error = "Generated hook anchor is missing";
        return false;
    }
    if (text.find(from, first + from.size()) != std::string::npos) {
        error = "Generated hook anchor is ambiguous";
        return false;
    }
    text.replace(first, from.size(), to);
    return true;
}

// tools/finalize_generated.py widens these comparisons from 0x1A8/0x140 to
// 0x228 so the culling paths keep the objects they were compiled for.
bool finalize_culling(const std::filesystem::path& path,
    const std::vector<uint32_t>& pcs, size_t single_match_pc, std::string& error) {
    std::string text;
    if (!read_file(path, text, error)) {
        return false;
    }
    for (const uint32_t pc : pcs) {
        char pattern_buffer[160];
        std::snprintf(pattern_buffer, sizeof(pattern_buffer),
            "(// 0x%08X: slti[^\\n]*\\n\\s*ctx->r1 = SIGNED\\(ctx->r[56]\\) < )0X(?:1A8|140)( \\? 1 : 0;)",
            pc);
        const std::regex pattern(pattern_buffer, std::regex::ECMAScript);

        std::string replaced;
        replaced.reserve(text.size());
        size_t cursor = 0;
        size_t count = 0;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern);
            it != std::sregex_iterator(); ++it) {
            const std::smatch& match = *it;
            replaced.append(text, cursor,
                static_cast<size_t>(match.position()) - cursor);
            // Group 1 ends right before the literal, group 2 starts at the
            // " ? 1 : 0;" suffix.
            replaced.append(match[1].str());
            replaced.append("0X228");
            replaced.append(match[2].str());
            cursor = static_cast<size_t>(match.position() + match.length());
            ++count;
        }
        replaced.append(text, cursor, std::string::npos);
        const size_t expected = pc == single_match_pc ? 1 : 2;
        if (count != expected) {
            char message[160];
            std::snprintf(message, sizeof(message),
                "Unexpected culling instruction count at %08X: %zu", pc, count);
            error = message;
            return false;
        }
        text = std::move(replaced);
    }
    return write_file(path, text, error);
}

}  // namespace

bool load_recipe(const std::filesystem::path& path, Recipe& recipe, std::string& error) {
    std::string contents;
    if (!read_file(path, contents, error)) {
        return false;
    }
    try {
        const nlohmann::json json = nlohmann::json::parse(contents);
        recipe.rom_sha256 = json.at("rom_sha256").get<std::string>();
        recipe.patched_rom_sha256 = json.at("patched_rom_sha256").get<std::string>();
        for (const auto& [name, checksum] : json.at("inputs").items()) {
            recipe.inputs.emplace(name, checksum.get<std::string>());
        }
        for (const auto& [name, checksum] : json.at("outputs").items()) {
            recipe.outputs.emplace(name, checksum.get<std::string>());
        }
    } catch (const std::exception& exception) {
        error = std::string("Invalid generated-code recipe: ") + exception.what();
        return false;
    }
    return true;
}

std::string canonical_tokens(std::string_view text) {
    const std::string sorted = sort_dispatch_runs(text);
    std::string output;
    output.reserve(sorted.size());

    size_t index = 0;
    while (index < sorted.size()) {
        const unsigned char value = static_cast<unsigned char>(sorted[index]);
        size_t end = index;

        if (is_space(value)) {
            ++index;
            continue;
        }
        if (value == '"' || value == '\'') {
            end = match_quoted(sorted, index, static_cast<char>(value));
        }
        else if (value == '/' && index + 1 < sorted.size() && sorted[index + 1] == '/') {
            end = sorted.find('\n', index);
            if (end == std::string::npos) {
                end = sorted.size();
            }
            index = end;
            continue;
        }
        else if (value == '/' && index + 1 < sorted.size() && sorted[index + 1] == '*') {
            end = sorted.find("*/", index + 2);
            end = end == std::string::npos ? sorted.size() : end + 2;
            index = end;
            continue;
        }
        else if (is_identifier_start(value)) {
            end = index + 1;
            while (end < sorted.size() &&
                is_identifier(static_cast<unsigned char>(sorted[end]))) {
                ++end;
            }
        }
        else if (is_digit(value)) {
            end = match_number(sorted, index);
        }
        else {
            end = match_operator(sorted, index);
        }

        output.append(sorted, index, end - index);
        output.push_back('\n');
        index = end;
    }
    if (!output.empty()) {
        output.pop_back();
    }
    return output;
}

std::string canonical_digest(std::string_view text) {
    return crypto::to_hex(crypto::Sha256::hash(canonical_tokens(text)));
}

bool finalize(const std::filesystem::path& directory, std::string& error) {
    std::filesystem::path path = directory / "funcs_1.c";
    std::string text;
    if (!read_file(path, text, error)) {
        return false;
    }
    if (!replace_once(text, "#include \"funcs.h\"",
            "#include \"funcs.h\"\n\nextern void wr64_forensic_capture_sky(uint8_t* rdram, uint32_t phase, uint32_t a0, uint32_t output_address);",
            error) ||
        !replace_once(text, "    { extern void wr64_sunny_sky_bottom_camera",
            "    const uint32_t wr64_forensic_a0 = (uint32_t)ctx->r4;\n"
            "    const uint32_t wr64_forensic_output = (uint32_t)ctx->r5;\n"
            "    wr64_forensic_capture_sky(rdram, 0, wr64_forensic_a0, wr64_forensic_output);\n"
            "    { extern void wr64_sunny_sky_bottom_camera",
            error) ||
        !replace_once(text, "    { extern void wr64_seabed_distance_finish",
            "    wr64_forensic_capture_sky(rdram, 1, wr64_forensic_a0, wr64_forensic_output);\n"
            "    { extern void wr64_seabed_distance_finish",
            error) ||
        !write_file(path, text, error)) {
        return false;
    }

    constexpr std::array<uint32_t, 11> kFuncs5Pcs = {
        0x8008C140, 0x8008C2EC, 0x8008C490, 0x8008C648,
        0x8008C7C8, 0x8008C7D8, 0x8008C960, 0x8008C970,
        0x8008CB14, 0x8008CCEC, 0x8008CE94,
    };
    if (!finalize_culling(directory / "funcs_5.c",
            std::vector<uint32_t>(kFuncs5Pcs.begin(), kFuncs5Pcs.end()),
            0x8008C960, error)) {
        return false;
    }
    if (!finalize_culling(directory / "funcs_6.c",
            {0x80094418, 0x800944A8, 0x8009453C}, 0x8009453C, error)) {
        return false;
    }

    path = directory / "funcs_20.c";
    if (!read_file(path, text, error)) {
        return false;
    }
    constexpr std::string_view kLocals =
        "    uint64_t hi = 0, lo = 0, result = 0;\n    int c1cs = 0;\n";
    struct EntryHook {
        std::string_view function;
        std::string_view hook;
    };
    constexpr std::array<EntryHook, 2> kEntryHooks = {{
        {"func_801FB514", "    uint32_t wr64HudScoreBegin = 0;\n"},
        {"func_i1_802C59E8",
            "    { extern void wr64_hud_park_spawn(void); wr64_hud_park_spawn(); }\n"},
    }};
    for (const EntryHook& entry : kEntryHooks) {
        const std::string signature = "RECOMP_FUNC void " + std::string(entry.function) +
            "(uint8_t* rdram, recomp_context* ctx) {\n";
        const std::string combined =
            signature + std::string(kLocals) + std::string(entry.hook);
        if (!replace_once(text, combined,
                signature + std::string(entry.hook) + std::string(kLocals), error)) {
            return false;
        }
    }
    return write_file(path, text, error);
}

bool patch_rsp_dispatch(const std::filesystem::path& path, std::string& error) {
    constexpr std::string_view kNeedle =
        "    // add         $2, $zero, $1\n"
        "    r2 = RSP_ADD32(0, r1);\n"
        "    // lh          $2, 0x10($2)\n"
        "    r2 = RSP_MEM_H_LOAD(0X10, r2);\n";
    constexpr std::string_view kReplacement =
        "    // The ABI command table at DMEM 0x10 is immutable, but later audio\n"
        "    // operations can alias that region in this recompilation. Dispatch directly\n"
        "    // from the command index to prevent corrupted, unaligned jump targets.\n"
        "    static constexpr uint16_t dispatch_targets[16] = {\n"
        "        0x1118, 0x1470, 0x11DC, 0x1B38,\n"
        "        0x1214, 0x187C, 0x1254, 0x12D0,\n"
        "        0x12EC, 0x1328, 0x140C, 0x1294,\n"
        "        0x1E24, 0x138C, 0x170C, 0x144C,\n"
        "    };\n"
        "    r2 = dispatch_targets[(r1 >> 1) & 0xF];\n";

    std::string text;
    if (!read_file(path, text, error)) {
        return false;
    }
    const size_t position = text.find(kNeedle);
    if (position == std::string::npos) {
        error = "aspMain dispatch block was not found";
        return false;
    }
    text.replace(position, kNeedle.size(), kReplacement);
    return write_file(path, text, error);
}

bool verify_generated(const std::filesystem::path& directory, const Recipe& recipe,
    std::string& error) {
    std::set<std::string> actual;
    for (const char* folder : {"RecompiledFuncsPoint0", "rsp"}) {
        const std::filesystem::path base = directory / folder;
        std::error_code ignored;
        if (!std::filesystem::is_directory(base, ignored)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(base, ignored)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string suffix = entry.path().extension().string();
            if (suffix == ".c" || suffix == ".cpp" || suffix == ".h" || suffix == ".inl") {
                actual.insert(entry.path().lexically_relative(directory).generic_string());
            }
        }
    }
    if (actual.size() != recipe.outputs.size()) {
        error = "Generated files are missing or unexpected. Run tools/generate_code.py "
            "with your ROM and recompiler tools.";
        return false;
    }
    for (const auto& [name, checksum] : recipe.outputs) {
        if (actual.count(name) == 0) {
            error = "Generated files are missing or unexpected. Run tools/generate_code.py "
                "with your ROM and recompiler tools.";
            return false;
        }
        std::string contents;
        if (!read_file(directory / name, contents, error)) {
            return false;
        }
        if (canonical_digest(contents) != checksum) {
            error = "Generated code does not match this release: " + name;
            return false;
        }
    }
    return true;
}

}  // namespace wr64::generation
