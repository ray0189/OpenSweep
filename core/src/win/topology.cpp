#include "topology.hpp"

#include <windows.h>

#include <bit>
#include <cstddef>
#include <memory>
#include <string>

#include "log.hpp"
#include "process_info.hpp"

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
                // PROCESSOR_RELATIONSHIP is { BYTE Flags; BYTE EfficiencyClass; BYTE Reserved[20]; ... }.
                // Older SDK / MinGW headers declare `BYTE Reserved[21]` instead of EfficiencyClass, so the
                // byte after Flags is read directly (documented ABI). Higher = more performant.
                const std::size_t cls = reinterpret_cast<const BYTE*>(&info->Processor.Flags)[1];
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

bool detect_cpu_sets(std::vector<commands2::CpuSetInfo>& out) {
    out.clear();
    using Fn = BOOL(WINAPI*)(void*, ULONG, PULONG, HANDLE, ULONG);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const auto fn = k32 ? reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(k32, "GetSystemCpuSetInformation")))
                        : nullptr;
    if (!fn) {
        log::warn("GetSystemCpuSetInformation unavailable; topology.cpuSets is empty");
        return false;
    }
    ULONG len = 0;
    fn(nullptr, 0, &len, nullptr, 0);
    if (len == 0 || len > 4u * 1024u * 1024u) {
        log::warn("GetSystemCpuSetInformation (size query) failed: " + log::win32_error(GetLastError()));
        return false;
    }
    std::unique_ptr<std::max_align_t[]> storage(new std::max_align_t[(len + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t)]);
    if (!fn(storage.get(), len, &len, nullptr, 0)) {
        log::warn("GetSystemCpuSetInformation failed: " + log::win32_error(GetLastError()));
        return false;
    }
    const auto* base = reinterpret_cast<const std::byte*>(storage.get());
    ULONG offset = 0;
    while (offset + 8 <= len) {
        const auto* e = reinterpret_cast<const procinfo::abi::CpuSetEntry*>(base + offset);
        if (e->Size == 0 || offset + e->Size > len) break;  // defensive: malformed entry
        if (e->Type == 0 && e->Size >= sizeof(procinfo::abi::CpuSetEntry)) {
            commands2::CpuSetInfo c;
            c.id = e->Id;
            c.group = e->Group;
            c.core = e->CoreIndex;
            c.efficiency_class = e->EfficiencyClass;
            c.numa = e->NumaNodeIndex;
            out.push_back(c);
        }
        offset += e->Size;
    }
    log::info("cpu sets: " + std::to_string(out.size()));
    return !out.empty();
}

}  // namespace opensweep::win
