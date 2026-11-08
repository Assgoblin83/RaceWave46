#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Generates libwr64game.so from the player's cartridge at first start. The
// packaged app ships no ROM-derived code: N64Recomp, RSPRecomp and Clang are
// run as child processes, and the module is compiled into the profile's data
// directory.
namespace wr64::game_builder {

struct Paths {
    // Stored ROM: either the untouched cartridge or the widescreen-patched
    // image librecomp saves after validation.
    std::filesystem::path rom;
    // <profile>/game-module, where prepared modules live.
    std::filesystem::path data_directory;
    // Holds config/, symbols/, data/, include/ and src/game_module/.
    std::filesystem::path source_root;
    std::filesystem::path compiler;
    std::filesystem::path n64recomp;
    std::filesystem::path rsprecomp;
    // Portable headers that must come before the system search path.
    std::vector<std::filesystem::path> header_directories;
    // Packaged glibc/libstdc++ headers, passed as -isystem.
    std::vector<std::filesystem::path> system_include_directories;
};

struct Progress {
    // Called from the build thread. Percent is 0..100.
    std::function<void(int percent, std::string_view step)> report;
    // Optional; return true to stop at the next checkpoint.
    std::function<bool()> cancelled;
};

struct Result {
    bool ok = false;
    std::filesystem::path module;
    std::filesystem::path log;
    std::string error;
};

// Resolves the packaged layout, with environment overrides for development:
// WR64_GAME_SOURCE_ROOT, WR64_GAME_TOOLS_ROOT, WR64_CLANG, WR64_N64RECOMP,
// WR64_RSPRECOMP, WR64_GAME_INCLUDE_DIRS and WR64_GAME_SYSTEM_INCLUDE_DIRS
// (the last two are colon-separated).
Paths default_paths(const std::filesystem::path& rom,
    const std::filesystem::path& data_directory);

// Builds the module, or reuses the one already prepared for this recipe and
// compiler. Calls are serialised.
Result build(const Paths& paths, const Progress& progress);

// Returns true and the module path when a matching module already exists.
bool prepared(const Paths& paths, std::filesystem::path& module);

}  // namespace wr64::game_builder
