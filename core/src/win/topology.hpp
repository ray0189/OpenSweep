// CPU topology via GetLogicalProcessorInformationEx (documented, read-only).
#pragma once

#include <vector>

#include "commands.hpp"
#include "commands2.hpp"

namespace opensweep::win {

// Fills `out` from RelationAll. Returns false (and logs) on failure.
bool detect_topology(commands::TopologyInfo& out);

// One entry per CPU set of the system (GetSystemCpuSetInformation, resolved at run time because
// not every SDK/MinGW header declares it). Returns false (out empty) when unavailable.
bool detect_cpu_sets(std::vector<commands2::CpuSetInfo>& out);

}  // namespace opensweep::win
