#pragma once

#include <string>
#include <vector>

namespace tool {

/// `game` empty means engine and unit suite only, and is passed to CMake as an empty
/// SUBSTRATE_GAME rather than omitted -- that is what clears a game a previous build-game
/// recorded.
int cmdBuild(const std::vector<std::string>& args, const std::string& game);

int cmdBuildGame(const std::vector<std::string>& args);

} // namespace tool
