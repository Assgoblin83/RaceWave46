#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

// C++ counterparts of tools/generate_code.py's checks and
// tools/finalize_generated.py. The application ships no Python, so the
// generation pipeline is reproduced here and kept in sync by a test suite.
namespace wr64::generation {

struct Recipe {
    std::string rom_sha256;
    std::string patched_rom_sha256;
    std::map<std::string, std::string> inputs;
    std::map<std::string, std::string> outputs;
};

bool load_recipe(const std::filesystem::path& path, Recipe& recipe, std::string& error);

// Exact port of canonical_tokens(): comments and formatting are ignored and
// unordered RSP dispatch cases are sorted.
std::string canonical_tokens(std::string_view text);
std::string canonical_digest(std::string_view text);

// Applies the host integration edits from tools/finalize_generated.py in
// place. Returns false with a descriptive error if an anchor has changed.
bool finalize(const std::filesystem::path& directory, std::string& error);

// Checks that the directory holds exactly the recipe's files and that every
// canonical digest matches.
bool verify_generated(const std::filesystem::path& directory, const Recipe& recipe,
    std::string& error);

}  // namespace wr64::generation
