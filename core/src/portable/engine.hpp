// OpenSweep Core engine: wires the Phase 2-6 modules into the loop of docs/core/phases-2-8.md
// ("Engine loop") and owns their persistence. Portable: no Windows headers. Owner: integration.
//
// Every tick (2 s, driven by the platform loop or by engine::Runner):
//   Trace.append + events -> Baselines.update -> Guard.evaluate (incidents; windows of newly opened
//   incidents are captured from the trace) -> SweepMemory / SweepScheduler.assess (fed with the
//   baselines, the guard's suspected-leak pids, the processes OpenSweep currently throttles) ->
//   ControlEngine.step (journal write-ahead, verification, rollback) -> SweepHeal.tick
//   -> periodic persistence.
//
// Persistence (all under `data_dir`, through the injected trace::FileIO):
//   trace.bin       every 5 min and at shutdown
//   baselines.json  every 15 min and at shutdown
//   events.jsonl    whenever new events arrived (append) and at shutdown
//   incidents.jsonl when an incident is created / re-opened / resolved, otherwise at most every 5 min
//   journal.jsonl   write-ahead, always (journal::Journal appends before every action)
//   config.json     mode, simulation, opt-ins, etw, boost; written on every successful setMode / setEtw /
//                   setBoost; validated on load (anything invalid -> fail-safe defaults: monitor, no
//                   simulation, no opt-ins, boost off)
//   scorecard.json  first start, scorecard baseline, stutter / fullscreen minutes (scorecard.hpp);
//                   every 15 min, on captureBaseline and at shutdown
// Game boost (boost.hpp) runs every tick before the control engine; recommendations for processes
// boost holds are dropped and boost never touches a process the control engine holds.
// start() loads everything and then rolls back whatever the journal says may still be applied
// (crash recovery: GameBoost::recover, then ControlEngine::recover) and expires interrupted recoveries
// (SweepHeal::recover). shutdown() ends a boost session, rolls back every active policy
// (ControlEngine::shutdown) and persists everything.
//
// Thread safety: every public method takes the engine mutex. tick() holds it for the duration of
// one tick (the expensive collection happens before, outside the lock); read methods copy the data
// out and the caller serialises it after the lock is released.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "baseline.hpp"
#include "boost.hpp"
#include "control.hpp"
#include "guard.hpp"
#include "heal.hpp"
#include "incident.hpp"
#include "journal.hpp"
#include "json.hpp"
#include "kernel_guard.hpp"
#include "memory_policy.hpp"
#include "scheduler_policy.hpp"
#include "scorecard.hpp"
#include "telemetry.hpp"
#include "trace.hpp"

namespace opensweep::engine {

using control::Mode;
using telemetry::EventRecord;
using telemetry::PolicyKind;
using telemetry::Sample;

// ---------------------------------------------------------------------------------------------
// config.json
struct Settings {
    Mode mode = Mode::Monitor;  // fail-safe default
    bool simulation = false;
    std::vector<PolicyKind> opted_in;
    bool etw = true;  // kernel tracing (docs/core/etw.md); absent in older files -> true
    bool boost = false;  // game boost opt-in (docs/core/alpha4.md); absent in older files -> false
};
inline constexpr std::int64_t kSettingsVersion = 1;
json::Value settings_to_json(const Settings& s);
// Strict: { version:1, mode:"monitor"|"balanced"|"advanced", simulation:bool, optIn:[kind...], etw?:bool,
//          boost?:bool }.
// Unknown keys, wrong types, unknown / duplicate kinds -> nullopt with `error` set.
std::optional<Settings> settings_from_json(const json::Value& v, std::string& error);

// journal::FileIO on top of a trace::FileIO (one injected file implementation for everything).
class JournalFileAdapter final : public journal::FileIO {
public:
    explicit JournalFileAdapter(trace::FileIO& io) : io_(io) {}
    bool append(const std::string& path, std::string_view data) override { return io_.append(path, data); }
    std::optional<std::string> read(const std::string& path) override;
    bool replace(const std::string& path, std::string_view data) override { return io_.write_atomic(path, data); }

private:
    trace::FileIO& io_;
};

// Joins a directory and a file name with the separator the directory already uses.
std::string join_path(std::string_view dir, std::string_view file);

struct EngineOptions {
    std::string data_dir;            // directory for all files (must exist)
    sched::Topology topology;        // logical count, hybrid flag, efficiency class per CPU set
    std::function<std::int64_t()> clock;           // wall clock (Unix ms) for admin calls; default system clock
    std::function<void(std::string_view)> log;     // optional diagnostics sink
    std::int64_t trace_persist_ms = 5 * 60'000;
    std::int64_t baseline_persist_ms = 15 * 60'000;
    std::int64_t incident_persist_ms = 5 * 60'000;  // touch-only incident updates
    std::size_t max_captures = 8;                    // incident windows kept in memory
    std::int64_t capture_before_ms = 5 * 60'000;     // trace window captured before an incident opens
    guard::GuardConfig guard;          // logical_processors is overridden from `topology`
    kguard::KernelGuardConfig kernel_guard;  // logical_processors is overridden from `topology`
    control::ControlConfig control;    // mode / simulation / opt-ins are overridden from config.json
    heal::HealConfig heal;
    memory::MemoryConfig memory;
    sched::SchedulerConfig scheduler;
    journal::JournalOptions journal;
    boost::BoostConfig boost;          // `lists` is overridden from `scheduler.lists`
    std::int64_t scorecard_persist_ms = 15 * 60'000;
};

inline constexpr const char* kTraceFile = "trace.bin";
inline constexpr const char* kEventsFile = "events.jsonl";
inline constexpr const char* kBaselinesFile = "baselines.json";
inline constexpr const char* kIncidentsFile = "incidents.jsonl";
inline constexpr const char* kJournalFile = "journal.jsonl";
inline constexpr const char* kConfigFile = "config.json";
inline constexpr const char* kDiagnosticsFile = "diagnostics.json";
inline constexpr const char* kScorecardFile = "scorecard.json";

struct IncidentCapture {
    std::string incident_id;
    std::int64_t captured_ms = 0;
    std::vector<trace::HiResRow> rows;  // <= capture_before_ms / 2 s rows
};

struct StartReport {
    trace::LoadReport trace, events, baselines, incidents;
    journal::LoadReport journal;
    bool config_found = false;
    bool config_valid = true;   // false: invalid file, defaults used
    std::string config_error;
    control::RollbackSummary recovered;  // policies rolled back by crash recovery
    control::RollbackSummary boost_recovered;  // boost targets restored by crash recovery
    trace::LoadReport scorecard;
    int heal_expired = 0;                // interrupted recoveries marked expired
};

struct Status {
    Mode mode = Mode::Monitor;
    bool simulation = true;              // decisions are simulated (monitor, or the simulation flag)
    bool simulation_flag = false;        // config flag (dry run in balanced / advanced)
    std::vector<PolicyKind> opted_in;
    bool running = false;
    std::uint64_t ticks = 0;
    std::int64_t last_tick_ms = 0;
    std::uint64_t tick_cost_us = 0;
    std::uint64_t tick_cost_max_us = 0;
    int stability_score = 100;
    std::vector<std::string> score_reasons;
    std::size_t incidents_open = 0;
    std::size_t policies_active = 0;
    std::size_t recommendations = 0;
    std::size_t pending_approvals = 0;
    bool breaker_open = false;
    bool no_action_needed = true;
    std::string decision_reason;
    std::string memory_level = "low";
    double memory_score = 0.0;
    bool cpu_contention = false;
    bool boost_enabled = false;
    bool boost_active = false;
};

struct BoostResult {
    bool enabled = false;
    bool active = false;
    bool persisted = false;  // config.json written
    control::RollbackSummary restored;  // targets restored because boost was turned off
};

struct ScorecardView {
    std::optional<scorecard::Baseline> baseline;
    scorecard::Metrics current;
    double hours_measured = 0.0;
};

struct CaptureResult {
    scorecard::Baseline baseline;
    bool persisted = false;  // scorecard.json written
};

struct HealView {
    bool open = false;
    int failures = 0;
    std::int64_t opened_at = 0;
    std::vector<heal::Approval> approvals;          // recovery actions awaiting approval
    std::vector<control::PendingConsent> consents;  // advanced-mode actions awaiting consent
    std::vector<journal::Transaction> history;      // recovery transactions, newest first
};

struct ModeResult {
    bool ok = false;
    std::string error;
    Settings settings;
    bool persisted = false;  // config.json written
    control::RollbackSummary rolled_back;
};

struct ApproveResult {
    enum class Kind { Recovery, Policy, Unknown, Refused };
    Kind kind = Kind::Unknown;
    std::string tx_id;
    std::string detail;
};

class Engine {
public:
    Engine(telemetry::Actuator& actuator, heal::Executor& executor, trace::FileIO& io, EngineOptions options);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Loads persisted state and performs crash recovery. Call once before the first tick.
    StartReport start();
    // One loop iteration (see header comment). `events` are the records collected since the last tick.
    void tick(const Sample& sample, std::span<const EventRecord> events);
    // Rolls back every active policy and persists everything. The engine stays usable afterwards.
    control::RollbackSummary shutdown();
    void set_running(bool on) noexcept { running_.store(on, std::memory_order_relaxed); }

    // Requests an OpenSweep recovery action (automatic ones run now, others become approvals).
    heal::RequestResult request_heal(const heal::RecoveryRequest& req);

    // ---- read side (copies) -----------------------------------------------------------------
    Status status() const;
    Mode mode() const noexcept { return mode_.load(std::memory_order_relaxed); }
    const char* mode_name() const noexcept { return control::to_string(mode()); }
    std::optional<Sample> latest() const;
    std::vector<trace::Point> history(trace::Metric m, trace::Range r) const;
    json::Value baselines_summary() const;
    std::vector<incident::Incident> incidents(std::size_t limit) const;
    std::vector<EventRecord> events(std::size_t limit) const;
    std::vector<control::Decision> recommendations() const;
    std::vector<control::ActivePolicy> policies() const;
    std::vector<journal::Transaction> journal(std::size_t limit) const;
    HealView heal(std::size_t history_limit) const;
    std::vector<IncidentCapture> captures() const;
    std::size_t tracked_processes() const;  // guard + scheduler + kernel guard bounded tracking (tests)
    std::vector<std::string> kernel_notes() const;  // why kernel-trace hogs were not candidates (last tick)
    boost::View boost_view() const;
    bool boost_enabled() const;
    ScorecardView scorecard() const;

    // Foreground hint (already validated by the platform). Feeds game boost's fullscreen detection.
    void report_foreground(std::uint32_t pid, bool fullscreen);

    // ---- admin side (the IPC layer has already checked the admin capability) ----------------
    // `simulation` / `opt_in` are optional additive arguments of setMode (see ipc-protocol.md).
    ModeResult set_mode(Mode m, std::optional<bool> simulation = std::nullopt,
                        std::optional<std::vector<PolicyKind>> opt_in = std::nullopt);
    control::RollbackSummary rollback_all();
    bool disable_policy(const std::string& tx_id);
    ApproveResult approve(const std::string& id);
    // Persists the kernel-tracing switch (config.json `etw`). The platform starts / stops the session.
    // Returns true when config.json was written.
    bool set_etw(bool enabled);
    bool etw_enabled() const;
    // Persists the game-boost opt-in (config.json `boost`); off ends an active session.
    BoostResult set_boost(bool enabled);
    // Stores the current scorecard metrics as the baseline ("before") in scorecard.json.
    CaptureResult capture_baseline();

    // Writes `data` atomically to data_dir/name; returns the full path or nullopt.
    std::optional<std::string> write_data_file(std::string_view name, std::string_view data);
    std::string data_path(std::string_view file) const { return join_path(options_.data_dir, file); }
    const EngineOptions& options() const noexcept { return options_; }

private:
    class OwnedExecutor;
    std::int64_t now() const;
    void log(std::string_view m) const;
    void record_config_event(std::int64_t now, std::string message);
    bool save_settings_locked(const Settings& s);
    void capture_incidents_locked(std::int64_t now);
    void propose_recoveries_locked(std::int64_t now);
    std::vector<std::uint32_t> suspected_leak_pids_locked() const;
    void persist_locked(std::int64_t now, bool force, bool incidents_changed, bool events_changed);
    void clear_caches_locked();
    void save_scorecard_locked();

    trace::FileIO& io_;
    EngineOptions options_;
    JournalFileAdapter jio_;

    struct LoadedSettings {
        Settings settings;
        bool found = false;
        bool valid = true;
        std::string error;
    };
    static LoadedSettings load_settings(trace::FileIO& io, const std::string& path);

    mutable std::mutex mu_;
    LoadedSettings cfg_;  // loaded before the modules below are constructed (control needs the mode)

    trace::Trace trace_;
    baseline::BaselineSet baselines_;
    incident::IncidentStore incidents_;
    guard::Guard guard_;
    kguard::KernelGuard kernel_guard_;
    memory::SweepMemory memory_;
    sched::SweepScheduler scheduler_;
    journal::Journal journal_;
    control::ControlEngine control_;
    boost::GameBoost boost_;
    scorecard::Scorecard scorecard_;
    std::unique_ptr<OwnedExecutor> owned_exec_;
    std::unique_ptr<heal::SweepHeal> heal_;

    std::atomic<Mode> mode_{Mode::Monitor};
    std::atomic<bool> running_{false};
    bool started_ = false;
    std::uint64_t ticks_ = 0;
    std::int64_t last_tick_ms_ = 0;
    std::uint64_t tick_cost_us_ = 0;
    std::uint64_t tick_cost_max_us_ = 0;

    std::vector<std::string> opened_;  // incidents opened by the last tick (guard + kernel guard)
    std::vector<std::string> kernel_notes_;
    std::vector<control::Decision> last_decisions_;
    bool last_no_action_ = true;
    std::string last_reason_;
    memory::PressureLevel mem_level_ = memory::PressureLevel::Low;
    double mem_score_ = 0.0;
    bool contention_ = false;
    std::deque<IncidentCapture> captures_;
    std::vector<std::pair<heal::Action, std::string>> heal_keys_;  // bounded: breaker aggregation

    std::int64_t last_trace_save_ = 0;
    std::int64_t last_baseline_save_ = 0;
    std::int64_t last_incident_save_ = 0;
    std::int64_t last_scorecard_save_ = 0;
    std::size_t last_incident_size_ = 0;
    std::size_t last_incident_open_ = 0;
    bool events_dirty_ = false;
};

// ---------------------------------------------------------------------------------------------
// Runner: drives an Engine from its own thread every `interval`. `collect` fills the sample and the
// new events (called outside the engine lock); returning false skips that tick. `after_tick` runs
// on the same thread (e.g. platform self-healing checks). stop() joins the thread and is called by
// the destructor.
class Runner {
public:
    using Collect = std::function<bool(Sample&, std::vector<EventRecord>&)>;
    Runner(Engine& engine, Collect collect, std::chrono::milliseconds interval,
           std::function<void()> after_tick = {});
    ~Runner();
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    void start();
    void stop();

private:
    void run();
    Engine& engine_;
    Collect collect_;
    std::chrono::milliseconds interval_;
    std::function<void()> after_tick_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace opensweep::engine
