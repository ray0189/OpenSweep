// CPU topology via GetLogicalProcessorInformationEx (documented, read-only).
#pragma once

#include "commands.hpp"

namespace opensweep::win {

// Fills `out` from RelationAll. Returns false (and logs) on failure.
bool detect_topology(commands::TopologyInfo& out);

}  // namespace opensweep::win
