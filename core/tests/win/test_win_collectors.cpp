// Windows-only tests for the collectors, the event collector and the actuator.
// These touch the real system (read-only, except the actuator tests, which only change a child
// process this test spawns and terminates). Built as `core_win_tests` (see core/CMakeLists.txt).
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "actuator.hpp"
#include "collectors.hpp"
#include "events.hpp"
#include "process_info.hpp"
#include "telemetry.hpp"
#include "test_framework.hpp"

using opensweep::telemetry::ApplyResult;
using opensweep::telemetry::EventKind;
using opensweep::telemetry::EventRecord;
using opensweep::telemetry::PolicyKind;
using opensweep::telemetry::PolicyTarget;
using opensweep::telemetry::ProcessSample;
using opensweep::telemetry::Sample;
using opensweep::win::EventCollector;
using opensweep::win::SystemCollector;
using opensweep::win::WinActuator;
namespace procinfo = opensweep::win::procinfo;

namespace {

std::uint32_t own_session() {
    std::uint32_t s = 0;
    procinfo::session_of(GetCurrentProcessId(), s);
    return s;
}

const ProcessSample* find_pid(const Sample& s, std::uint32_t pid) {
    for (const ProcessSample& p : s.top)
        if (p.pid == pid) return &p;
    return nullptr;
}

bool contains(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }

// A harmless child process (ping to localhost for ~30 s) that the actuator tests may modify.
struct Child {
    PROCESS_INFORMATION pi{};
    PolicyTarget target;
    bool started = false;

    Child() {
        wchar_t sysdir[MAX_PATH];
        const UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return;
        std::wstring cmd = L"\"" + std::wstring(sysdir, n) + L"\\PING.EXE\" -n 30 127.0.0.1";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        // Explicit NORMAL priority: CI runners may start jobs below normal and children inherit it.
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS,
                            nullptr, nullptr, &si, &pi)) {
            std::fprintf(stderr, "  CreateProcess(ping) failed: %lu\n", GetLastError());
            return;
        }
        started = true;
        target.pid = pi.dwProcessId;
        target.name = "PING.EXE";
        procinfo::creation_time_ms(pi.hProcess, target.process_start_ms);
        // Memory priority is inherited too; reset it to normal (5) so the actuator has room to lower it.
        struct { ULONG MemoryPriority; } mpi{5};  // MEMORY_PRIORITY_NORMAL (mirrored: SDK guards vary)
        if (!SetProcessInformation(pi.hProcess, ProcessMemoryPriority, &mpi, sizeof(mpi)))
            std::printf("  note: could not reset child memory priority: %lu\n", GetLastError());
        std::printf("  child priority class=0x%lx\n", GetPriorityClass(pi.hProcess));
        Sleep(200);  // let the process finish initialising
    }
    void kill() {
        if (!started) return;
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    ~Child() {
        kill();
        if (pi.hThread) CloseHandle(pi.hThread);
        if (pi.hProcess) CloseHandle(pi.hProcess);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
};

WinActuator::Options test_options() {
    WinActuator::Options o;
    o.allow_session0 = true;  // CI runner processes may live in session 0
    return o;
}

// apply(value) -> read back -> restore(original) -> read back original.
void round_trip(WinActuator& act, PolicyKind kind, const PolicyTarget& t, std::int64_t value) {
    std::int64_t before = 0;
    std::string err;
    const bool readable = act.read(kind, t, before, &err);
    if (!readable) std::printf("  note: %s not readable before apply: %s\n", opensweep::telemetry::to_string(kind), err.c_str());

    const ApplyResult r = act.apply(kind, t, value);
    if (!r.ok) std::fprintf(stderr, "  apply(%s) error: %s\n", opensweep::telemetry::to_string(kind), r.error.c_str());
    CHECK(r.ok);
    if (!r.ok) return;
    if (readable) CHECK_EQ(r.original, before);

    std::int64_t now = 0;
    if (act.read(kind, t, now, &err)) {
        CHECK_EQ(now, value);
    } else {
        std::printf("  note: %s not readable after apply: %s\n", opensweep::telemetry::to_string(kind), err.c_str());
    }

    const ApplyResult rr = act.restore(kind, t, r.original);
    if (!rr.ok) std::fprintf(stderr, "  restore(%s) error: %s\n", opensweep::telemetry::to_string(kind), rr.error.c_str());
    CHECK(rr.ok);
    if (act.read(kind, t, now, &err)) CHECK_EQ(now, r.original);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// SystemCollector

TEST(win_collector_two_samples_plausible) {
    SystemCollector c;
    Sample s1, s2;
    CHECK(c.collect(s1));
    Sleep(2000);
    CHECK(c.collect(s2));

    CHECK(s2.ts_ms > s1.ts_ms);
    CHECK(s2.mem_total > 0);
    CHECK(s2.mem_avail > 0 && s2.mem_avail <= s2.mem_total);
    CHECK(s2.mem_load_percent <= 100);
    CHECK(s2.commit_limit > 0 && s2.commit_total > 0);
    CHECK(s2.cpu_percent >= 0.0 && s2.cpu_percent <= 100.0);
    CHECK(s2.processes > 0);
    CHECK(s2.threads > 0);
    CHECK(s2.handles > 0);
    CHECK(s2.compressed_bytes == 0);  // documented: no supported source
    CHECK(!s2.top.empty());
    CHECK(s2.top.size() <= 40);
    for (const ProcessSample& p : s2.top) {
        CHECK(p.cpu_percent >= 0.0 && p.cpu_percent <= 100.0);
        CHECK(!p.name.empty());
    }

    const ProcessSample* self = find_pid(s2, GetCurrentProcessId());
    CHECK(self != nullptr);
    if (self) {
        CHECK(self->private_bytes > 0);
        CHECK(self->working_set > 0);
        CHECK(self->start_ms > 0);
        CHECK(self->threads > 0);
        CHECK(self->handles > 0);
        CHECK(self->priority_class != 0);
        CHECK_EQ(procinfo::normalize_name(self->name), std::string("core_win_tests"));
        CHECK(!self->is_security_software);
    }
    // pid 4 (System), if listed, must be marked critical.
    if (const ProcessSample* sys = find_pid(s2, 4)) CHECK(sys->is_system_critical);

    std::printf("  cpu=%.1f%% queue=%.1f ctxsw/s=%.0f dpc=%.2f%% isr=%.2f%% hardflt/s=%.1f pgflt/s=%.1f\n",
                s2.cpu_percent, s2.cpu_queue_length, s2.context_switches_per_sec, s2.dpc_percent,
                s2.interrupt_percent, s2.hard_faults_per_sec, s2.page_faults_per_sec);
    std::printf("  disk r=%.2fms w=%.2fms q=%.2f B/s=%.0f freq=%.1f%% procs=%u top=%zu\n", s2.disk_read_latency_ms,
                s2.disk_write_latency_ms, s2.disk_queue_length, s2.disk_bytes_per_sec, s2.cpu_freq_percent,
                s2.processes, s2.top.size());
    std::printf("  pdh counters ok=%zu missing=%zu; collect cost last=%llu us max=%llu us\n", c.counters_ok(),
                c.counters_missing(), static_cast<unsigned long long>(c.last_collect_us()),
                static_cast<unsigned long long>(c.max_collect_us()));
    CHECK(c.last_collect_us() > 0);
    CHECK(c.last_collect_us() < 5000000);  // sanity only; the < 20 ms target is measured, not asserted
}

TEST(win_collector_missing_counter_does_not_fail) {
    SystemCollector::Options o;
    o.extra_counters.push_back(L"\\OpenSweep Nonexistent Object(_Total)\\Nothing Here");
    SystemCollector c(o);
    CHECK(c.counters_missing() >= 1);
    Sample s;
    CHECK(c.collect(s));
    Sleep(200);
    CHECK(c.collect(s));
    CHECK(s.mem_total > 0);
}

TEST(win_collector_cost_repeated) {
    SystemCollector c;
    Sample s;
    for (int i = 0; i < 5; ++i) {
        CHECK(c.collect(s));
        Sleep(100);
    }
    std::printf("  5 collects: last=%llu us max=%llu us top=%zu\n",
                static_cast<unsigned long long>(c.last_collect_us()), static_cast<unsigned long long>(c.max_collect_us()),
                s.top.size());
    CHECK(s.top.size() <= 40);
}

TEST(win_collector_foreground_hint) {
    SystemCollector c;
    const std::uint32_t self = GetCurrentProcessId();
    const std::uint32_t session = own_session();
    CHECK(!c.report_foreground(0));
    CHECK(!c.report_foreground(4));
    CHECK(!c.report_foreground(self, session + 1));  // wrong caller session
    const bool accepted = c.report_foreground(self, session);
    CHECK_EQ(accepted, session != 0);  // session 0 is never an interactive foreground
    if (accepted) {
        CHECK_EQ(c.foreground_pid(), self);
        Sample s;
        CHECK(c.collect(s));
        CHECK_EQ(s.foreground_pid, self);
        const ProcessSample* p = find_pid(s, self);
        CHECK(p != nullptr && p->foreground);
    } else {
        CHECK_EQ(c.foreground_pid(), 0u);
        std::printf("  note: test runs in session 0; foreground hint correctly refused\n");
    }
}

// ---------------------------------------------------------------------------------------------
// EventCollector

TEST(win_events_classify_and_sanitize) {
    EventKind k = EventKind::Other;
    CHECK(EventCollector::classify("Application Error", 1000, "", k) && k == EventKind::AppCrash);
    CHECK(EventCollector::classify("Application Hang", 1002, "", k) && k == EventKind::AppHang);
    CHECK(EventCollector::classify("Windows Error Reporting", 1001, "APPCRASH", k) && k == EventKind::AppCrash);
    CHECK(!EventCollector::classify("Windows Error Reporting", 1001, "StoreAgentScanForUpdatesFailure0", k));
    CHECK(EventCollector::classify("Service Control Manager", 7031, "", k) && k == EventKind::ServiceFailure);
    CHECK(EventCollector::classify("Service Control Manager", 7026, "", k) && k == EventKind::DriverFailure);
    CHECK(!EventCollector::classify("Service Control Manager", 7036, "", k));
    CHECK(EventCollector::classify("Microsoft-Windows-Kernel-Power", 41, "", k) && k == EventKind::UnexpectedShutdown);
    CHECK(EventCollector::classify("EventLog", 6008, "", k) && k == EventKind::UnexpectedShutdown);
    CHECK(EventCollector::classify("Microsoft-Windows-WER-SystemErrorReporting", 1001, "", k) && k == EventKind::Bugcheck);
    CHECK(EventCollector::classify("Microsoft-Windows-WHEA-Logger", 17, "", k) && k == EventKind::Whea);
    CHECK(EventCollector::classify("Display", 4101, "", k) && k == EventKind::GpuTimeout);
    CHECK(EventCollector::classify("disk", 153, "", k) && k == EventKind::DiskError);
    CHECK(EventCollector::classify("Microsoft-Windows-WindowsUpdateClient", 20, "", k) && k == EventKind::WindowsUpdate);
    CHECK(EventCollector::classify("Microsoft-Windows-Diagnostics-Performance", 100, "", k) &&
          k == EventKind::BootPerformance);
    CHECK(EventCollector::classify("Microsoft-Windows-Resource-Exhaustion-Detector", 2004, "", k) &&
          k == EventKind::ResourceExhaustion);

    CHECK_EQ(EventCollector::sanitize_subject("C:\\Program Files\\App\\app.exe", true), std::string("app.exe"));
    CHECK_EQ(EventCollector::sanitize_subject("\\Driver\\WudfRd", true), std::string("WudfRd"));
    CHECK_EQ(EventCollector::sanitize_subject("a\r\nb", false), std::string("a b"));
    CHECK(EventCollector::sanitize_subject(std::string(500, 'x'), false).size() <= 96);

    const std::wstring q = EventCollector::build_query(86400000, true);
    CHECK(q.find(L"&lt;= 86400000") != std::wstring::npos);
    CHECK(q.find(L"<Select Path=\"Application\">") != std::wstring::npos);
    CHECK(EventCollector::build_query(86400000, false).find(L"timediff") == std::wstring::npos);
}

TEST(win_events_backfill) {
    EventCollector ec;
    std::vector<EventRecord> out;
    const bool ok = ec.poll(out);
    CHECK(ok);
    CHECK(ec.subscribed());
    CHECK(ec.backfill_done());
    CHECK(out.size() <= 500);
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    const std::int64_t now = procinfo::filetime_to_unix_ms(procinfo::filetime_u64(ft));
    for (const EventRecord& r : out) {
        CHECK(!r.message.empty());
        CHECK(r.message.size() <= 200);
        CHECK(r.level >= 1 && r.level <= 4);
        CHECK(r.ts_ms >= now - 24LL * 3600 * 1000 - 60000 && r.ts_ms <= now + 60000);
    }
    std::printf("  backfill: %zu record(s), dropped=%llu\n", out.size(),
                static_cast<unsigned long long>(ec.records_dropped()));
    for (std::size_t i = 0; i < out.size() && i < 5; ++i)
        std::printf("    [%s] %s %u: %s\n", opensweep::telemetry::to_string(out[i].kind), out[i].provider.c_str(),
                    out[i].event_id, out[i].message.c_str());
    std::vector<EventRecord> more;
    CHECK(ec.poll(more));  // live phase: usually empty
}

// ---------------------------------------------------------------------------------------------
// WinActuator

TEST(win_actuator_round_trips_on_child) {
    Child child;
    CHECK(child.started);
    if (!child.started) return;
    WinActuator act(test_options());
    CHECK(act.alive(child.target));
    round_trip(act, PolicyKind::EcoQoS, child.target, 1);
    round_trip(act, PolicyKind::MemoryPriority, child.target, 2);
    round_trip(act, PolicyKind::PriorityClass, child.target, BELOW_NORMAL_PRIORITY_CLASS);

    if (WinActuator::efficiency_cpu_set_ids().empty()) {
        const ApplyResult r = act.apply(PolicyKind::CpuSets, child.target, 2);
        CHECK(!r.ok);
        CHECK(contains(r.error, "hybrid"));
        std::printf("  note: not a hybrid CPU; CPU set apply correctly refused\n");
    } else {
        round_trip(act, PolicyKind::CpuSets, child.target, 2);
    }
}

TEST(win_actuator_guards) {
    Child child;
    CHECK(child.started);
    if (!child.started) return;
    WinActuator act(test_options());

    PolicyTarget t = child.target;
    t.process_start_ms -= 1000;  // PID reuse guard
    ApplyResult r = act.apply(PolicyKind::PriorityClass, t, BELOW_NORMAL_PRIORITY_CLASS);
    CHECK(!r.ok && contains(r.error, "start time"));
    r = act.restore(PolicyKind::PriorityClass, t, NORMAL_PRIORITY_CLASS);
    CHECK(!r.ok && contains(r.error, "start time"));
    CHECK(!act.alive(t));

    t = child.target;
    t.process_start_ms = 0;
    r = act.apply(PolicyKind::EcoQoS, t, 1);
    CHECK(!r.ok && contains(r.error, "start time"));

    t = child.target;
    t.name = "explorer.exe";  // protection list (by name, before opening)
    r = act.apply(PolicyKind::EcoQoS, t, 1);
    CHECK(!r.ok && contains(r.error, "protection list"));
    t.name = "MsMpEng.exe";
    r = act.apply(PolicyKind::MemoryPriority, t, 2);
    CHECK(!r.ok && contains(r.error, "protection list"));
    t.name = "notping.exe";  // real image name differs
    r = act.apply(PolicyKind::EcoQoS, t, 1);
    CHECK(!r.ok && contains(r.error, "mismatch"));

    PolicyTarget sys;
    sys.pid = 4;
    sys.name = "System";
    sys.process_start_ms = 1;
    r = act.apply(PolicyKind::EcoQoS, sys, 1);
    CHECK(!r.ok && contains(r.error, "refused"));

    PolicyTarget self;
    self.pid = GetCurrentProcessId();
    procinfo::creation_time_ms(GetCurrentProcess(), self.process_start_ms);
    r = act.apply(PolicyKind::EcoQoS, self, 1);
    CHECK(!r.ok && contains(r.error, "own process"));

    // Values outside the allowed direction / range.
    CHECK(!act.apply(PolicyKind::EcoQoS, child.target, 0).ok);
    CHECK(!act.apply(PolicyKind::MemoryPriority, child.target, 5).ok);
    CHECK(!act.apply(PolicyKind::MemoryPriority, child.target, 1).ok);
    CHECK(!act.apply(PolicyKind::PriorityClass, child.target, HIGH_PRIORITY_CLASS).ok);
    CHECK(!act.restore(PolicyKind::PriorityClass, child.target, REALTIME_PRIORITY_CLASS).ok);
    CHECK(!act.restore(PolicyKind::MemoryPriority, child.target, 9).ok);

    // Session-0 refusal with default options.
    std::uint32_t child_session = 0;
    if (procinfo::session_of(child.target.pid, child_session) && child_session == 0) {
        WinActuator strict;
        r = strict.apply(PolicyKind::EcoQoS, child.target, 1);
        CHECK(!r.ok && contains(r.error, "session 0"));
    }

    // Exited process: every call is safe and reports failure.
    child.kill();
    CHECK(!act.alive(child.target));
    r = act.apply(PolicyKind::EcoQoS, child.target, 1);
    CHECK(!r.ok);
    r = act.restore(PolicyKind::EcoQoS, child.target, -1);
    CHECK(!r.ok);
    std::int64_t v = 0;
    CHECK(!act.read(PolicyKind::MemoryPriority, child.target, v));
}

TEST(win_procinfo_lists) {
    CHECK(procinfo::is_protected_name("LSASS.EXE"));
    CHECK(procinfo::is_protected_name("Memory Compression"));
    CHECK(procinfo::is_protected_name("svchost"));
    CHECK(procinfo::is_protected_name("EasyAntiCheat.exe"));
    CHECK(!procinfo::is_protected_name("notepad.exe"));
    CHECK(!procinfo::is_protected_name(""));
    CHECK(procinfo::is_security_software_name("MsMpEng.exe"));
    CHECK(!procinfo::is_security_software_name("chrome.exe"));
    wchar_t win[MAX_PATH];
    const UINT n = GetSystemWindowsDirectoryW(win, MAX_PATH);
    CHECK(n > 0);
    CHECK(procinfo::is_under_system_root(std::wstring(win, n) + L"\\System32\\ping.exe"));
    CHECK(!procinfo::is_under_system_root(L"D:\\Games\\game.exe"));
}
