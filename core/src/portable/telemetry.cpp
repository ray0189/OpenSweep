#include "telemetry.hpp"

namespace opensweep::telemetry {

const char* to_string(WorkloadClass c) noexcept {
    switch (c) {
        case WorkloadClass::LatencyCritical: return "LatencyCritical";
        case WorkloadClass::Interactive: return "Interactive";
        case WorkloadClass::Throughput: return "Throughput";
        case WorkloadClass::Background: return "Background";
        case WorkloadClass::Efficiency: return "Efficiency";
        case WorkloadClass::Unknown: break;
    }
    return "Unknown";
}

const char* to_string(EventKind k) noexcept {
    switch (k) {
        case EventKind::AppCrash: return "app_crash";
        case EventKind::AppHang: return "app_hang";
        case EventKind::ServiceFailure: return "service_failure";
        case EventKind::UnexpectedShutdown: return "unexpected_shutdown";
        case EventKind::Bugcheck: return "bugcheck";
        case EventKind::Whea: return "whea";
        case EventKind::DriverFailure: return "driver_failure";
        case EventKind::GpuTimeout: return "gpu_timeout";
        case EventKind::DiskError: return "disk_error";
        case EventKind::WindowsUpdate: return "windows_update";
        case EventKind::Thermal: return "thermal";
        case EventKind::BootPerformance: return "boot_performance";
        case EventKind::ResourceExhaustion: return "resource_exhaustion";
        case EventKind::OpenSweepConfig: return "opensweep_config";
        case EventKind::Other: break;
    }
    return "other";
}

const char* to_string(PolicyKind k) noexcept {
    switch (k) {
        case PolicyKind::EcoQoS: return "ecoqos";
        case PolicyKind::MemoryPriority: return "memory_priority";
        case PolicyKind::PriorityClass: return "priority_class";
        case PolicyKind::CpuSets: return "cpu_sets";
    }
    return "unknown";
}

}  // namespace opensweep::telemetry
