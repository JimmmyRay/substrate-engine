#pragma once

#include <string>
#include <vector>

namespace tool {

/// RenderDoc capture and analysis. The capture and the conversion are here; the XML walk
/// stays in scripts/rdoc/analyse.py, which this drives.
int cmdRdoc(const std::vector<std::string>& args);

int cmdInstallHooks(const std::vector<std::string>& args);

} // namespace tool
