#pragma once

#include <filesystem>
#include <string>

#include "wr64_game_module.h"

// Loads the ROM-derived game code from a shared library instead of linking it
// into the executable. Only compiled when WR64_GAME_MODULE is defined.
namespace wr64::game_module {

// The module shipped beside the executable in a development build. Release
// builds pass the path of the module prepared from the player's ROM.
std::filesystem::path default_path();

// Loads and validates the ABI. Returns false and sets last_error() on failure;
// a previous successful load is kept.
bool load(const std::filesystem::path& path);
bool loaded();
const Wr64GameModule& abi();
const std::string& last_error();
void unload();

}  // namespace wr64::game_module
