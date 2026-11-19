// Phase 2-6 IPC commands (docs/core/phases-2-8.md "IPC additions", docs/core/ipc-protocol.md).
// Handlers copy data out of the Engine (short critical sections) and serialise afterwards.
// Argument validation is strict: unknown argument keys, wrong types and out-of-range values are
// `bad_args`. Admin commands require Capability::Admin (granted by the pipe server only to elevated
// Administrators); the dispatcher rejects other callers with `forbidden` before any argument check.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "commands.hpp"
#include "engine.hpp"
#include "json.hpp"
#include "kernel_stats.hpp"
#include "protocol.hpp"

namespace opensweep::commands2 {

struct CpuSetInfo {
    std::uint32_t id = 0;
    std::uint32_t group = 0;
    std::uint32_t core = 0;
    std::uint32_t efficiency_class = 0;
    std::uint32_t numa = 0;
};

// Result of a foreground hint check, decided by the platform from the *token* session of the caller.
enum class ForegroundVerdict {
    Accepted,        // recorded as the foreground hint
    Ignored,         // valid but meaningless (e.g. caller and pid in session 0): accepted, not recorded
    NoSuchProcess,   // -> bad_args
    WrongSession,    // pid exists but not in the caller's session -> bad_args
    Unavailable,     // the platform cannot check (e.g. not implemented) -> internal
};

struct Platform {
    commands::SystemProvider* provider = nullptr;   // Phase 1 topology fields
    std::vector<CpuSetInfo> cpu_sets;               // static, detected once
    // (pid, caller session from the client's token) -> verdict. Never receives client-chosen sessions.
    std::function<ForegroundVerdict(std::uint32_t pid, std::uint32_t caller_session)> report_foreground;
    // Extra platform data for exportDiagnostics (e.g. collector counters). Optional.
    std::function<json::Value()> diagnostics_extra;
    // Service identity for the diagnostics bundle.
    const commands::ServiceInfo* service = nullptr;
    // Kernel tracing (docs/core/etw.md). All optional: without them `latency` / `diskActivity` /
    // `hardFaults` report an empty, not-running view and `setEtw` only persists the switch.
    std::function<kstats::EtwStatus()> etw_status;
    std::function<kstats::LatencyView()> etw_latency;                     // last 10 min, top 20
    std::function<std::vector<kstats::DiskProcessView>()> etw_disk;       // last 2 min, top 20
    std::function<std::vector<kstats::FaultProcessView>()> etw_faults;    // last 2 min, top 20
    std::function<kstats::EtwStatus(bool enabled)> set_etw;               // starts / stops the session
};

// ---- JSON builders (exposed for tests and the diagnostics bundle) ------------------------------
json::Value sample_to_json(const telemetry::Sample& s);
json::Value process_to_json(const telemetry::ProcessSample& p);
json::Value event_to_json(const telemetry::EventRecord& e);       // { tsMs, kind, provider, eventId, level, subject, message, value }
json::Value status_to_json(const engine::Status& st);
json::Value kernel_sample_json(const telemetry::Sample& s);       // { valid, running, dpcPercent, ... } (sample.kernel)
json::Value decision_to_json(const control::Decision& d);
json::Value policy_to_json(const control::ActivePolicy& p);
json::Value approval_to_json(const heal::Approval& a);
json::Value consent_to_json(const control::PendingConsent& c);
json::Value heal_to_json(const engine::HealView& h);
json::Value baselines_to_json(const json::Value& summary);       // drops metrics without data yet
json::Value topology_to_json(const commands::TopologyInfo& t, const std::vector<CpuSetInfo>& sets);
json::Value build_diagnostics(engine::Engine& engine, const Platform& platform);

// Limits (response-size safety; every list result is additionally trimmed to fit 1 MiB).
inline constexpr std::size_t kDefaultIncidents = 50, kMaxIncidents = 500;
inline constexpr std::size_t kDefaultEvents = 100, kMaxEvents = 2000;
inline constexpr std::size_t kDefaultJournal = 100, kMaxJournal = 500;
inline constexpr std::size_t kHealHistory = 50;
inline constexpr std::size_t kListBudgetBytes = 900 * 1024;

// Registers every command of the contract table, re-registers `topology` (Phase 1 shape + cpuSets).
// hello/health report the engine mode when register_phase1 was given engine_mode_provider(engine).
void register_engine_commands(protocol::Dispatcher& dispatcher, engine::Engine& engine, const Platform& platform);
commands::ModeProvider engine_mode_provider(const engine::Engine& engine);

}  // namespace opensweep::commands2
