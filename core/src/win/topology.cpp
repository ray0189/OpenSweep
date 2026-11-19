#include "topology.hpp"

#include <windows.h>

#include <bit>
#include <cstddef>
#include <memory>
#include <string>

#include "log.hpp"

namespace opensweep::win {

namespace {

std::uint32_t popcount_affinity(KAFFINITY mask) noexcept {
    return static_cast<std::uint32_t>(std::popcount(static_cast<unsigned long long>(mask)));
}

}  // namespace

bool detect_topology(commands::TopologyInfo& out) {
    out = commands::TopologyInfo{};

    DWORD len = 0;
    if (GetLogicalProcessorInformationEx(RelationAll, nullptr, &len) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        len == 0) {
        log::error("GetLogicalProcessorInformationEx (size query) failed: " + log::win32_error(GetLastError()));
        return false;
    }
    // The structures contain pointer-sized members; allocate with suitable alignment.
    std::unique_ptr<std::max_align_t[]> storage;
    for (int attempt = 0; attempt < 3; ++attempt) {
        storage.reset(new std::max_align_t[(len + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t)]);
        if (GetLogicalProcessorInformationEx(RelationAll,
                                             reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.get()),
                                             &len))
            break;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || attempt == 2) {
            log::error("GetLogicalProcessorInformationEx failed: " + log::win32_error(GetLastError()));
            return false;
        }
        // Topology changed between calls (e.g. hot-add); retry with the new size.
    }

    std::uint32_t logical_from_cores = 0;
    std::uint32_t logical_from_groups = 0;
    const auto* base = reinterpret_cast<const std::byte*>(storage.get());
    DWORD offset = 0;
    while (offset < len) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(base + offset);
        if (info->Size == 0 || offset + info->Size > len) break;  // defensive: malformed entry
        switch (info->Relationship) {
            case RelationProcessorCore: {
                ++out.cores;
                const std::size_t cls = info->Processor.EfficiencyClass;  // higher = more performant (documented)
                if (out.efficiency_classes.size() <= cls) out.efficiency_classes.resize(cls + 1, 0);
                ++out.efficiency_classes[cls];
                for (WORD g = 0; g < info->Processor.GroupCount; ++g)
                    logical_from_cores += popcount_affinity(info->Processor.GroupMask[g].Mask);
                break;
            }
            case RelationProcessorPackage:
                ++out.packages;
                break;
            case RelationNumaNode:
                ++out.numa_nodes;
                break;
            case RelationGroup: {
                out.groups = info->Group.ActiveGroupCount;
                for (WORD g = 0; g < info->Group.ActiveGroupCount; ++g)
                    logical_from_groups += info->Group.GroupInfo[g].ActiveProcessorCount;
                break;
            }
            case RelationCache:
                if (info->Cache.Level == 3) ++out.l3_caches;
                break;
            default:
                break;
        }
        offset += info->Size;
    }
    out.logical_processors = logical_from_groups != 0 ? logical_from_groups : logical_from_cores;

    log::info("topology: logical=" + std::to_string(out.logical_processors) + " cores=" + std::to_string(out.cores) +
              " packages=" + std::to_string(out.packages) + " numa=" + std::to_string(out.numa_nodes) +
              " groups=" + std::to_string(out.groups) + " l3=" + std::to_string(out.l3_caches) +
              " hybrid=" + (out.hybrid() ? "yes" : "no"));
    return out.logical_processors > 0;
}

}  // namespace opensweep::win
