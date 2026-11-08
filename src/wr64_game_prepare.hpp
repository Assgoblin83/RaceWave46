#pragma once

namespace wr64::game_prepare {

// Registers the launcher hooks that turn "Start Game" into "Prepare Game"
// until the module for the stored ROM exists. Called from frontend configure.
void initialize();

}  // namespace wr64::game_prepare
