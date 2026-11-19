// Shared data model for Phases 2-8 (observability, stability, resource intelligence, optimisation,
// self-healing). Portable: no Windows headers. Windows collectors (src/win/collectors*) fill these
// structs; the portable engine (trace, baseline, guard, memory_policy, scheduler_policy, control,
// journal, heal) consumes them. See docs/core/phases-2-8.md for the contract.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opensweep::telemetry {

// Workload classes from Part 5 of the master prompt.
enum class WorkloadClass { LatencyCritical, Interactive, Throughput, Background, Efficiency, Unknown };

// One process as seen in one sample. Never contains window titles, command lines or file contents.
struct ProcessSample {
    std::uint32_t pid = 0;
    std::uint32_t parent_pid = 0;
    std::string name;                // image file name only, e.g. "chrome.exe"
    double cpu_percent = 0.0;        // of the whole machine, 0..100, over the last interval
    std::uint64_t private_bytes = 0;
    std::uint64_t working_set = 0;
    std::uint64_t hard_faults_per_sec = 0;  // 0 when not available
    std::uint32_t handles = 0;
    std::uint32_t threads = 0;
    std::uint32_t session_id = 0;
    std::uint32_t priority_class = 0;   // Win32 priority class value, 0 if unknown
    bool foreground = false;            // owns the foreground window (hint from the client, see docs)
    bool is_system_critical = false;    // protected / critical / session 0 Windows component
    bool is_security_software = false;  // AV/EDR (by known image list or protected light process)
    bool is_service = false;
    bool has_audio = false;             // currently rendering audio (0 = unknown/false)
    bool signed_by_microsoft = false;
    // Added by win-collect (appended; existing fields unchanged):
    std::int64_t start_ms = 0;          // process creation time, Unix epoch ms; feeds PolicyTarget::process_start_ms
    double page_faults_per_sec = 0.0;   // all page faults (soft + hard) from PageFaultCount deltas;
                                        // per-process *hard* faults are not exposed by a documented
                                        // non-ETW API, so hard_faults_per_sec stays 0
};

// Kernel tracing (ETW, docs/core/etw.md) per-sample data. Values cover the interval since the
// previous sample (normally 2 s). Names are driver file names / process image names only.
struct KernelDriverSample {
    std::string name;            // e.g. "ndis.sys"; "unknown" when the routine address is not mapped
    double max_us = 0.0;         // longest single DPC or ISR in the interval
    double total_us = 0.0;       // DPC + ISR execution time in the interval
    std::uint64_t count = 0;     // DPCs + ISRs
};

struct KernelProcessSample {
    std::uint32_t pid = 0;
    std::string name;
    double read_bps = 0.0;            // disk bytes/s issued by this process (DiskIo completions)
    double write_bps = 0.0;
    double mean_latency_ms = 0.0;     // mean HighResResponseTime of its I/Os
    double max_latency_ms = 0.0;
    double hard_faults_per_sec = 0.0; // hard page faults attributed via thread -> process
};

struct KernelSummary {
    bool running = false;             // the kernel session delivered this interval
    double dpc_percent = 0.0;         // DPC time / (interval x logical CPUs) x 100
    double isr_percent = 0.0;
    double worst_dpc_us = 0.0;
    double worst_isr_us = 0.0;
    double hard_faults_per_sec = 0.0; // all attributed + unattributed hard faults
    double disk_bytes_per_sec = 0.0;  // all DiskIo completions
    std::uint64_t events_lost = 0;    // cumulative, since the session started
    double cost_percent = 0.0;        // consumer CPU, % of one CPU (rolling 30 s)
};

inline constexpr std::size_t kMaxKernelDrivers = 24;    // bound of Sample::kernel_drivers
inline constexpr std::size_t kMaxKernelProcesses = 24;  // bound of Sample::kernel_processes

// One system-wide sample, taken every `kSampleIntervalMs`.
struct Sample {
    std::int64_t ts_ms = 0;              // Unix epoch ms UTC
    double cpu_percent = 0.0;            // 0..100
    double cpu_queue_length = 0.0;       // \System\Processor Queue Length (runnable threads waiting)
    double context_switches_per_sec = 0.0;
    double dpc_percent = 0.0;            // % DPC time (all CPUs)
    double interrupt_percent = 0.0;      // % interrupt time
    std::uint64_t mem_total = 0;         // bytes
    std::uint64_t mem_avail = 0;
    std::uint32_t mem_load_percent = 0;
    std::uint64_t commit_total = 0;
    std::uint64_t commit_limit = 0;
    std::uint64_t system_cache = 0;
    std::uint64_t kernel_paged = 0;
    std::uint64_t kernel_nonpaged = 0;
    std::uint64_t compressed_bytes = 0;  // memory compression store size, 0 if unknown
    double hard_faults_per_sec = 0.0;    // \Memory\Page Reads/sec (hard faults that hit disk)
    double page_faults_per_sec = 0.0;
    double disk_read_latency_ms = 0.0;   // \PhysicalDisk(_Total)\Avg. Disk sec/Read * 1000
    double disk_write_latency_ms = 0.0;
    double disk_queue_length = 0.0;
    double disk_bytes_per_sec = 0.0;
    double cpu_freq_percent = 100.0;     // \Processor Information(_Total)\% Processor Performance; <100 can mean throttling
    std::uint32_t processes = 0;
    std::uint32_t threads = 0;
    std::uint32_t handles = 0;
    std::uint32_t foreground_pid = 0;    // 0 = unknown
    std::vector<ProcessSample> top;      // top processes by CPU and by private bytes (bounded, <= 40)
    // Added by the kernel tracing work (appended; existing fields unchanged). Empty / not valid when
    // the ETW session is off or not running.
    bool kernel_valid = false;
    KernelSummary kernel;
    std::vector<KernelDriverSample> kernel_drivers;     // <= kMaxKernelDrivers, largest total first
    std::vector<KernelProcessSample> kernel_processes;  // <= kMaxKernelProcesses, disk or fault activity
};

// A Windows event relevant to stability. Collected from the System/Application logs and the
// Kernel-* providers. `message` is a short, privacy-safe summary (no document text, no user data).
enum class EventKind {
    AppCrash,             // Application Error 1000, WER 1001
    AppHang,              // Application Hang 1002
    ServiceFailure,       // Service Control Manager 7031/7034/7023/7024
    UnexpectedShutdown,   // Kernel-Power 41, EventLog 6008
    Bugcheck,             // BugCheck 1001
    Whea,                 // WHEA-Logger
    DriverFailure,        // e.g. 219, 7026, Kernel-PnP failures
    GpuTimeout,           // Display 4101 (TDR)
    DiskError,            // disk 7/11/51/153, Ntfs 55/98, storport
    WindowsUpdate,        // WindowsUpdateClient 19/20/43
    Thermal,              // Kernel-Power 86/125 / ACPI thermal
    BootPerformance,      // Diagnostics-Performance 100 (boot duration)
    ResourceExhaustion,   // Resource-Exhaustion-Detector 2004
    OpenSweepConfig,      // OpenSweep's own configuration change (recorded by the engine)
    Other
};

struct EventRecord {
    std::int64_t ts_ms = 0;
    EventKind kind = EventKind::Other;
    std::string provider;     // e.g. "Service Control Manager"
    std::uint32_t event_id = 0;
    std::uint8_t level = 4;   // 1 critical, 2 error, 3 warning, 4 info
    std::string subject;      // the app / service / driver / disk involved, if any (name only)
    std::string message;      // short summary, <= 200 chars
    double value = 0.0;       // numeric payload when meaningful (e.g. boot duration ms)
};

inline constexpr int kSampleIntervalMs = 2000;

const char* to_string(WorkloadClass c) noexcept;
const char* to_string(EventKind k) noexcept;

// ---------------------------------------------------------------------------------------------
// Actuator: the only way the portable engine can change anything on the system. The Windows
// implementation uses documented APIs only (SetProcessInformation for EcoQoS / memory priority,
// SetPriorityClass, SetProcessDefaultCpuSets). Every apply returns the original value so that the
// journal can roll it back. All methods must be safe to call on a process that has exited.
enum class PolicyKind {
    EcoQoS,          // ProcessPowerThrottling EXECUTION_SPEED on (value 1) / off (0) / default (-1)
    MemoryPriority,  // ProcessMemoryPriority, 1..5 (MEMORY_PRIORITY_VERY_LOW..NORMAL)
    PriorityClass,   // Win32 priority class value
    CpuSets          // value = count of efficiency-class-0 CPU sets used; detail lists ids
};

struct PolicyTarget {
    std::uint32_t pid = 0;
    std::string name;
    std::int64_t process_start_ms = 0;  // guards against PID reuse; 0 = unknown
};

struct ApplyResult {
    bool ok = false;
    std::int64_t original = 0;   // value before the change (for rollback)
    std::string error;           // empty on success
};

class Actuator {
public:
    virtual ~Actuator() = default;
    virtual ApplyResult apply(PolicyKind kind, const PolicyTarget& target, std::int64_t value) = 0;
    virtual ApplyResult restore(PolicyKind kind, const PolicyTarget& target, std::int64_t original) = 0;
    // Returns false if the process no longer exists or the start time no longer matches.
    virtual bool alive(const PolicyTarget& target) = 0;
};

const char* to_string(PolicyKind k) noexcept;

}  // namespace opensweep::telemetry
