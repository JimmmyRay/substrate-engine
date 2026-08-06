#pragma once

#include <string>
#include <vector>

namespace tool {

/// Exit 1 is an image difference, 2 is a harness failure. The split is load-bearing: only
/// one of them is news about the change under test.
int cmdGolden(const std::vector<std::string>& args);

int cmdReadback(const std::vector<std::string>& args);

int cmdLocomotion(const std::vector<std::string>& args);
int cmdArena(const std::vector<std::string>& args);

} // namespace tool
