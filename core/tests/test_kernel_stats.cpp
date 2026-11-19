// Kernel tracing aggregation (kernel_stats): address mapping, attribution, windows / bounds,
// histograms, per-sample rates, self-protection (cost / loss) and the IPC JSON shapes.
#include <cmath>
#include <string>

#include "json.hpp"
#include "kernel_stats.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using kstats::KernelStats;
using kstats::Routine;

namespace {

constexpr std::int64_t kT0 = 1'779'999'980'000;  // 20 s into a minute
constexpr std::uint64_t kNt = 0xFFFFF80000000000ull;
constexpr std::uint64_t kNdis = 0xFFFFF80010000000ull;
constexpr std::uint64_t kNv = 0xFFFFF80020000000ull;

KernelStats loaded(unsigned cpus = 4) {
    kstats::Config c;
    c.logical_processors = cpus;
    KernelStats k(c);
    k.on_image_load(kNt, 0x1000000, "\\SystemRoot\\system32\\ntoskrnl.exe");
    k.on_image_load(kNdis, 0x200000, "\\SystemRoot\\System32\\drivers\\ndis.sys");
    k.on_image_load(kNv, 0x4000000, "\\SystemRoot\\System32\\DriverStore\\FileRepository\\nv\\nvlddmkm.sys");
    return k;
}

const kstats::DriverStats* find_driver(const kstats::LatencyView& v, const std::string& name) {
    for (const auto& d : v.drivers)
        if (d.name == name) return &d;
    return nullptr;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps * std::max(1.0, std::fabs(b)); }

}  // namespace

TEST(kstats_histogram_buckets) {
    CHECK_EQ(kstats::histogram_bucket(0.0), 0u);
    CHECK_EQ(kstats::histogram_bucket(49.9), 0u);
    CHECK_EQ(kstats::histogram_bucket(50.0), 1u);
    CHECK_EQ(kstats::histogram_bucket(99.0), 1u);
    CHECK_EQ(kstats::histogram_bucket(249.0), 2u);
    CHECK_EQ(kstats::histogram_bucket(499.0), 3u);
    CHECK_EQ(kstats::histogram_bucket(999.0), 4u);
    CHECK_EQ(kstats::histogram_bucket(1999.0), 5u);
    CHECK_EQ(kstats::histogram_bucket(3999.0), 6u);
    CHECK_EQ(kstats::histogram_bucket(4000.0), 7u);
    CHECK_EQ(kstats::histogram_bucket(1e9), 7u);
}

TEST(kstats_address_mapping) {
    KernelStats k = loaded();
    CHECK_EQ(k.images(), 3u);
    CHECK_EQ(k.driver_for(kNdis), std::string("ndis.sys"));                 // file name only, no path
    CHECK_EQ(k.driver_for(kNdis + 0x1FFFFF), std::string("ndis.sys"));
    CHECK_EQ(k.driver_for(kNdis + 0x200000), std::string("unknown"));        // end is exclusive
    CHECK_EQ(k.driver_for(kNt + 5), std::string("ntoskrnl.exe"));
    CHECK_EQ(k.driver_for(kNv + 0x3000000), std::string("nvlddmkm.sys"));
    CHECK_EQ(k.driver_for(0x1000), std::string("unknown"));
    CHECK_EQ(k.driver_for(0xFFFFFFFFFFFFFFFFull), std::string("unknown"));

    // Unload removes the range; a new image at an overlapping address replaces the old one.
    k.on_image_unload(kNdis);
    CHECK_EQ(k.driver_for(kNdis + 10), std::string("unknown"));
    k.on_image_load(kNv + 0x100, 0x1000, "other.sys");
    CHECK_EQ(k.driver_for(kNv + 0x200), std::string("other.sys"));
    CHECK_EQ(k.driver_for(kNv + 0x3000000), std::string("unknown"));  // nvlddmkm range was replaced
    // Rundown repeat of the same image: one entry, same name id.
    k.on_image_load(kNdis, 0x200000, "NDIS.SYS");
    k.on_image_load(kNdis, 0x200000, "ndis.sys");
    CHECK_EQ(k.driver_for(kNdis + 1), std::string("ndis.sys"));

    // Malformed ranges are rejected, never crash.
    const auto before = k.counters().malformed;
    k.on_image_load(0xFFFFFFFFFFFFFF00ull, 0x1000, "wrap.sys");
    k.on_image_load(kNdis, 0, "zero.sys");
    CHECK_EQ(k.counters().malformed, before + 2);
}

TEST(kstats_dpc_isr_attribution_and_histogram) {
    KernelStats k = loaded(4);
    const std::int64_t t = kT0;
    k.on_dpc_isr(Routine::Dpc, kNdis + 0x100, 30.0, t);
    k.on_dpc_isr(Routine::Dpc, kNdis + 0x200, 120.0, t + 1);
    k.on_dpc_isr(Routine::Isr, kNdis + 0x300, 4500.0, t + 2);
    k.on_dpc_isr(Routine::Dpc, kNv + 0x10, 700.0, t + 3);
    k.on_dpc_isr(Routine::Dpc, 0x42, 10.0, t + 4);  // unmapped
    const auto v = k.latency(t + 1000);
    const auto* nd = find_driver(v, "ndis.sys");
    CHECK(nd != nullptr);
    if (nd) {
        CHECK_EQ(nd->dpc_count, 2u);
        CHECK_EQ(nd->isr_count, 1u);
        CHECK(near(nd->total_us(), 4650.0));
        CHECK(near(nd->max_us, 4500.0));
        CHECK_EQ(nd->histogram[0], 1u);
        CHECK_EQ(nd->histogram[2], 1u);
        CHECK_EQ(nd->histogram[7], 1u);
    }
    CHECK(find_driver(v, "nvlddmkm.sys") != nullptr);
    CHECK(find_driver(v, "unknown") != nullptr);
    CHECK_EQ(k.counters().unknown_routine, 1u);
    // Worst first.
    CHECK_EQ(v.drivers.front().name, std::string("ndis.sys"));
    CHECK(near(v.worst_isr_us, 4500.0));
    CHECK(near(v.worst_dpc_us, 700.0));
    // Covered 1 s on 4 CPUs: DPC 860 us / 4e6 us = 0.0215 %.
    CHECK_EQ(v.covered_ms, 1000);
    CHECK(near(v.dpc_percent, 860.0 / 4e6 * 100.0, 1e-9));
    CHECK(near(v.isr_percent, 4500.0 / 4e6 * 100.0, 1e-9));
}

TEST(kstats_rejects_implausible_durations) {
    KernelStats k = loaded();
    k.on_dpc_isr(Routine::Dpc, kNdis, -1.0, kT0);
    k.on_dpc_isr(Routine::Dpc, kNdis, NAN, kT0);
    k.on_dpc_isr(Routine::Dpc, kNdis, INFINITY, kT0);
    k.on_dpc_isr(Routine::Isr, kNdis, 2e6, kT0);
    k.on_disk_io(1, 0xFFFFFFFFu, 4096, false, -5.0, kT0);
    k.on_disk_io(1, 0xFFFFFFFFu, 1ull << 40, false, 1.0, kT0);
    CHECK_EQ(k.counters().malformed, 6u);
    CHECK_EQ(k.counters().dpc_isr, 0u);
    CHECK(k.latency(kT0 + 1).drivers.empty());
}

TEST(kstats_disk_and_fault_attribution) {
    KernelStats k = loaded();
    k.on_process_start(1000, "backup.exe");
    k.on_process_start(2000, "game.exe");
    k.on_thread_start(11, 1000);
    k.on_thread_start(12, 1000);
    k.on_thread_start(21, 2000);
    const std::int64_t t = kT0;
    k.on_disk_io(11, 0xFFFFFFFFu, 1'000'000, false, 2.0, t);
    k.on_disk_io(12, 0xFFFFFFFFu, 3'000'000, true, 6.0, t + 10);
    k.on_disk_io(21, 0xFFFFFFFFu, 500'000, false, 1.0, t + 20);
    k.on_disk_io(99, 2000, 100, false, 1.0, t + 30);          // unknown thread: header pid fallback
    k.on_disk_io(98, 0xFFFFFFFFu, 100, false, 1.0, t + 40);   // unknown thread, no fallback
    CHECK_EQ(k.counters().unattributed, 1u);
    for (int i = 0; i < 50; ++i) k.on_hard_fault(21, 0xFFFFFFFFu, t + 50);
    for (int i = 0; i < 10; ++i) k.on_hard_fault(11, 0xFFFFFFFFu, t + 60);

    const auto disk = k.disk_activity(t + 10'000);
    CHECK(disk.size() >= 2);
    CHECK_EQ(disk[0].pid, 1000u);
    CHECK_EQ(disk[0].name, std::string("backup.exe"));
    CHECK(near(disk[0].read_bps, 1'000'000 / 10.0));
    CHECK(near(disk[0].write_bps, 3'000'000 / 10.0));
    CHECK_EQ(disk[0].ios, 2u);
    CHECK(near(disk[0].mean_latency_ms, 4.0));
    CHECK(near(disk[0].max_latency_ms, 6.0));
    CHECK_EQ(disk[1].pid, 2000u);
    CHECK_EQ(disk[1].ios, 2u);  // its own I/O + the header-pid fallback

    const auto f = k.hard_faults(t + 10'000);
    CHECK_EQ(f.size(), 2u);
    CHECK_EQ(f[0].pid, 2000u);
    CHECK(near(f[0].faults_per_sec, 5.0));
    CHECK(near(f[1].faults_per_sec, 1.0));

    // Thread end: further I/O on that tid is no longer attributed to the process.
    k.on_thread_end(11);
    CHECK_EQ(k.pid_for_thread(11), kstats::kOtherPid);
    CHECK_EQ(k.pid_for_thread(12), 1000u);
}

TEST(kstats_name_resolver_called_once_and_long_names) {
    KernelStats k = loaded();
    int calls = 0;
    k.set_name_resolver([&](std::uint32_t pid) {
        ++calls;
        return pid == 7000 ? std::string("SearchProtocolHost.exe") : std::string();
    });
    k.on_process_start(7000, "SearchProtocolH");  // kernel image names are cut at 15 characters
    CHECK_EQ(k.process_name(7000), std::string("SearchProtocolHost.exe"));
    CHECK_EQ(calls, 1);
    CHECK_EQ(k.process_name(8000), std::string("unknown"));  // resolver fails
    CHECK_EQ(k.process_name(8000), std::string("unknown"));  // cached: not asked again
    CHECK_EQ(calls, 2);
    CHECK_EQ(k.process_name(4), std::string("System"));
}

TEST(kstats_windows_are_bounded_and_rolled) {
    KernelStats k = loaded(1);
    // 90 minutes of one DPC per 10 s.
    for (std::int64_t t = kT0; t < kT0 + 90 * 60'000; t += 10'000) k.on_dpc_isr(Routine::Dpc, kNdis, 100.0, t);
    CHECK(k.closed_windows() <= kstats::kMaxWindows);
    CHECK_EQ(k.closed_windows(), kstats::kMaxWindows);
    for (const auto& w : k.windows()) {
        CHECK_EQ(w.end_ms - w.start_ms, kstats::kWindowMs);
        CHECK_EQ(w.start_ms % kstats::kWindowMs, 0);
    }
    // Last 10 min (+ the open window): about 60 DPCs + the current partial minute.
    const std::int64_t now = kT0 + 90 * 60'000;
    const auto v = k.latency(now, 10 * 60'000);
    const auto* nd = find_driver(v, "ndis.sys");
    CHECK(nd != nullptr);
    if (nd) {
        CHECK(nd->dpc_count >= 60 && nd->dpc_count <= 72);
    }
    CHECK(v.covered_ms >= 10 * 60'000 && v.covered_ms <= 11 * 60'000);
}

TEST(kstats_top_keys_per_window) {
    KernelStats k = loaded();
    const std::int64_t t = kT0;
    for (std::uint32_t pid = 100; pid < 200; ++pid) {
        k.on_thread_start(pid * 10, pid);
        k.on_disk_io(pid * 10, 0xFFFFFFFFu, 1000ull * pid, false, 1.0, t);
    }
    k.advance(t + 2 * 60'000);  // closes the window
    CHECK_EQ(k.closed_windows(), 1u);
    const auto& w = k.windows().front();
    CHECK_EQ(w.disk.size(), kstats::kTopKeysPerWindow);
    CHECK_EQ(w.keys_dropped, 100u - kstats::kTopKeysPerWindow);
    CHECK_EQ(w.disk.front().pid, 199u);  // largest first
    CHECK(w.disk.back().pid >= 136u);    // the smallest 36 were dropped
}

TEST(kstats_bounded_maps) {
    KernelStats k;
    for (std::uint32_t tid = 1; tid <= kstats::kMaxThreads + 100; ++tid) k.on_thread_start(tid, 5);
    CHECK(k.threads() <= kstats::kMaxThreads);
    CHECK(k.counters().map_resets >= 1);
    for (std::uint32_t pid = 1; pid <= kstats::kMaxProcesses + 10; ++pid) k.on_process_start(pid * 4, "a.exe");
    CHECK(k.processes() <= kstats::kMaxProcesses);
    for (std::uint64_t i = 0; i < kstats::kMaxImages + 10; ++i)
        k.on_image_load(kNt + i * 0x10000, 0x1000, "d" + std::to_string(i) + ".sys");
    CHECK(k.images() <= kstats::kMaxImages);
    CHECK(k.kernel_images_named() <= kstats::kMaxDriverNames);
}

TEST(kstats_fill_sample_rates_and_reset) {
    KernelStats k = loaded(2);
    k.on_process_start(1000, "backup.exe");
    k.on_thread_start(11, 1000);
    const std::int64_t t = kT0;
    telemetry::Sample s0;
    k.fill_sample(t, s0, true, 0.1);  // starts the first interval
    for (int i = 0; i < 40; ++i) k.on_dpc_isr(Routine::Dpc, kNdis, 50.0, t + 100 + i);
    k.on_dpc_isr(Routine::Isr, kNv, 1500.0, t + 200);
    k.on_disk_io(11, 0xFFFFFFFFu, 8'000'000, true, 10.0, t + 300);
    for (int i = 0; i < 300; ++i) k.on_hard_fault(11, 0xFFFFFFFFu, t + 400);
    k.set_events_lost(7);

    telemetry::Sample s;
    k.fill_sample(t + 2000, s, true, 0.4);
    CHECK(s.kernel_valid);
    CHECK(s.kernel.running);
    CHECK_EQ(s.kernel.events_lost, 7u);
    CHECK(near(s.kernel.cost_percent, 0.4));
    CHECK(near(s.kernel.dpc_percent, 2000.0 / (2.0 * 1e6 * 2) * 100.0, 1e-9));
    CHECK(near(s.kernel.isr_percent, 1500.0 / (2.0 * 1e6 * 2) * 100.0, 1e-9));
    CHECK(near(s.kernel.worst_isr_us, 1500.0));
    CHECK(near(s.kernel.hard_faults_per_sec, 150.0));
    CHECK(near(s.kernel.disk_bytes_per_sec, 4'000'000.0));
    CHECK_EQ(s.kernel_drivers.size(), 2u);
    CHECK(s.kernel_drivers.size() <= telemetry::kMaxKernelDrivers);
    CHECK_EQ(s.kernel_drivers[0].name, std::string("ndis.sys"));  // largest total first
    CHECK_EQ(s.kernel_processes.size(), 1u);
    if (!s.kernel_processes.empty()) {
        const auto& p = s.kernel_processes[0];
        CHECK_EQ(p.pid, 1000u);
        CHECK(near(p.write_bps, 4'000'000.0));
        CHECK(near(p.hard_faults_per_sec, 150.0));
        CHECK(near(p.mean_latency_ms, 10.0));
    }
    // Next interval starts empty.
    telemetry::Sample s2;
    k.fill_sample(t + 4000, s2, false, 0.0);
    CHECK(!s2.kernel_valid);
    CHECK(s2.kernel_drivers.empty());
    CHECK(s2.kernel_processes.empty());
    CHECK_EQ(s2.kernel.dpc_percent, 0.0);
}

TEST(kstats_fill_sample_bounds) {
    KernelStats k(kstats::Config{8, kstats::kTopKeysPerWindow, kstats::kMaxWindows});
    for (std::uint64_t i = 0; i < 200; ++i) {
        k.on_image_load(kNt + i * 0x100000, 0x100000, "drv" + std::to_string(i) + ".sys");
        k.on_dpc_isr(Routine::Dpc, kNt + i * 0x100000 + 5, static_cast<double>(i + 1), kT0);
        k.on_thread_start(static_cast<std::uint32_t>(10000 + i), static_cast<std::uint32_t>(500 + i));
        k.on_disk_io(static_cast<std::uint32_t>(10000 + i), 0xFFFFFFFFu, 1000 + i, false, 1.0, kT0);
        k.on_hard_fault(static_cast<std::uint32_t>(10000 + i), 0xFFFFFFFFu, kT0);
    }
    telemetry::Sample s;
    k.fill_sample(kT0 + 2000, s, true, 0.0);
    CHECK_EQ(s.kernel_drivers.size(), telemetry::kMaxKernelDrivers);
    CHECK(s.kernel_processes.size() <= telemetry::kMaxKernelProcesses);
    CHECK_EQ(s.kernel_drivers[0].name, std::string("drv199.sys"));
}

TEST(kstats_self_protection_cost) {
    kstats::SelfProtection sp;
    using V = kstats::SelfProtection::Verdict;
    std::uint64_t cpu = 0;
    // 1 % of one CPU (10 ms per second) for 60 s: fine.
    for (int i = 0; i <= 60; ++i) {
        CHECK(sp.update(i * 1000, cpu, 0) == V::Ok);
        cpu += 10'000;
    }
    CHECK(sp.cost_percent() > 0.9 && sp.cost_percent() < 1.1);
    // Spike to 5 % for less than the window: still Ok until 30 s of it.
    sp.reset();
    cpu = 0;
    V last = V::Ok;
    int first_bad = -1;
    for (int i = 0; i <= 40; ++i) {
        last = sp.update(i * 1000, cpu, 0);
        if (last == V::TooExpensive && first_bad < 0) first_bad = i;
        cpu += 50'000;
    }
    CHECK_EQ(first_bad, 30);  // needs a full 30 s window
    CHECK(last == V::TooExpensive);
    CHECK(sp.cost_percent() > 4.9 && sp.cost_percent() < 5.1);
}

TEST(kstats_self_protection_loss) {
    using V = kstats::SelfProtection::Verdict;
    kstats::SelfProtection sp;
    // Losing events every second for 30 s -> LosingEvents.
    int first_bad = -1;
    for (int i = 0; i <= 40; ++i) {
        if (sp.update(i * 1000, 0, static_cast<std::uint64_t>(i) * 5) == V::LosingEvents && first_bad < 0) first_bad = i;
    }
    CHECK(first_bad >= 29 && first_bad <= 31);
    // Intermittent loss (every other second) never counts as continuous.
    sp.reset();
    std::uint64_t lost = 0;
    bool bad = false;
    for (int i = 0; i <= 120; ++i) {
        if (i % 2 == 0) lost += 3;
        if (sp.update(i * 1000, 0, lost) != V::Ok) bad = true;
    }
    CHECK(!bad);
    CHECK_EQ(std::string(kstats::to_string(V::LosingEvents)), std::string("losing_events"));
}

TEST(kstats_flags_levels) {
    using namespace kstats::flag;
    CHECK_EQ(kstats::flags_for_level(0), kProcess | kThread | kImageLoad | kDpc | kInterrupt | kDiskIo | kHardFaults);
    CHECK_EQ(kstats::flags_for_level(1) & (kDpc | kInterrupt), 0u);
    CHECK((kstats::flags_for_level(1) & kDiskIo) != 0);
    CHECK_EQ(kstats::flags_for_level(2) & kDiskIo, 0u);
    CHECK((kstats::flags_for_level(2) & kHardFaults) != 0);
    CHECK_EQ(kstats::flags_for_level(3), 0u);
    const auto n = kstats::flag_names(kstats::flags_for_level(0));
    CHECK_EQ(n.size(), 7u);
    CHECK_EQ(n.front(), std::string("PROCESS"));
    CHECK_EQ(n.back(), std::string("HARD_FAULTS"));
    CHECK_EQ(kstats::driver_description("NDIS.SYS"), std::string("network driver"));
    CHECK(kstats::driver_description("mystery.sys").empty());
}

TEST(kstats_json_shapes) {
    KernelStats k = loaded(4);
    k.on_dpc_isr(Routine::Dpc, kNdis, 300.0, kT0);
    k.on_process_start(1000, "backup.exe");
    k.on_thread_start(11, 1000);
    k.on_disk_io(11, 0xFFFFFFFFu, 4096, true, 2.5, kT0);
    k.on_hard_fault(11, 0xFFFFFFFFu, kT0);
    kstats::EtwStatus st;
    st.running = true;
    st.flags = kstats::flags_for_level(0);
    st.events_lost = 3;
    st.cost_percent = 0.123456;
    const json::Value lat = kstats::latency_json(st, k.latency(kT0 + 1000));
    const json::Value* status = lat.find("status");
    CHECK(status && status->is_object());
    if (status) {
        CHECK(status->find("running") && status->find("running")->as_bool());
        CHECK(status->find("flags") && status->find("flags")->size() == 7);
        CHECK(status->find("eventsLost") && status->find("eventsLost")->as_int() == 3);
        CHECK(status->find("costPercent") && near(status->find("costPercent")->as_double(), 0.12));
        CHECK(!status->find("reason"));
    }
    const json::Value* sys = lat.find("system");
    CHECK(sys && sys->find("dpcPercent") && sys->find("isrPercent") && sys->find("worstDpcUs") &&
          sys->find("worstIsrUs"));
    const json::Value* drivers = lat.find("drivers");
    CHECK(drivers && drivers->size() == 1);
    if (drivers && drivers->size() == 1) {
        const json::Value& d = drivers->as_array()[0];
        for (const char* key : {"name", "dpcCount", "isrCount", "totalUs", "maxUs", "histogram"}) CHECK(d.find(key));
        CHECK(d.find("histogram")->size() == kstats::kHistogramBuckets);
        CHECK(d.find("description") && d.find("description")->as_string() == "network driver");
    }
    const json::Value disk = kstats::disk_activity_json(k.disk_activity(kT0 + 1000));
    CHECK(disk.find("processes") && disk.find("processes")->size() == 1);
    if (disk.find("processes") && disk.find("processes")->size() == 1) {
        const json::Value& p = disk.find("processes")->as_array()[0];
        for (const char* key : {"pid", "name", "readBps", "writeBps", "ios", "meanLatencyMs", "maxLatencyMs"})
            CHECK(p.find(key));
    }
    const json::Value hf = kstats::hard_faults_json(k.hard_faults(kT0 + 1000));
    CHECK(hf.find("processes") && hf.find("processes")->size() == 1);
    if (hf.find("processes") && hf.find("processes")->size() == 1) {
        const json::Value& p = hf.find("processes")->as_array()[0];
        for (const char* key : {"pid", "name", "faultsPerSec"}) CHECK(p.find(key));
    }
    // Not running: flags reported empty, reason present.
    kstats::EtwStatus off;
    off.flags = kstats::flags_for_level(0);
    off.reason = "disabled";
    const json::Value o = kstats::etw_status_json(off);
    CHECK(o.find("flags")->size() == 0);
    CHECK(o.find("reason") && o.find("reason")->as_string() == "disabled");
    CHECK(json::is_valid_utf8(json::serialize(lat)));
}
