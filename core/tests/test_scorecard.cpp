// Scorecard (scorecard.hpp): metrics from synthetic trace history, nulls, events, persistence and the
// engine's first-start / automatic baseline capture.
#include <cmath>
#include <string>
#include <vector>

#include "engine.hpp"
#include "engine_test_util.hpp"
#include "scorecard.hpp"
#include "test_framework.hpp"
#include "trace.hpp"

using namespace opensweep;
using namespace eng_test;

namespace {

constexpr std::int64_t kMin = 60'000;
constexpr std::int64_t kDay = 24LL * 3600 * 1000;

telemetry::Sample plain(std::int64_t ts, double cpu, std::uint64_t avail) {
    telemetry::Sample s;
    s.ts_ms = ts;
    s.cpu_percent = cpu;
    s.mem_total = 16 * obs_test::kGBu;
    s.mem_avail = avail;
    s.disk_read_latency_ms = 2.0;
    s.disk_write_latency_ms = 4.0;
    s.hard_faults_per_sec = 3.0;
    return s;
}

bool near(const std::optional<double>& v, double expect, double tol = 1e-6) {
    return v && std::fabs(*v - expect) <= tol;
}

telemetry::EventRecord ev(std::int64_t ts, telemetry::EventKind k, double value = 0.0) {
    telemetry::EventRecord e;
    e.ts_ms = ts;
    e.kind = k;
    e.provider = "test";
    e.value = value;
    return e;
}

}  // namespace

TEST(scorecard_idle_metrics_from_synthetic_history) {
    trace::Trace tr;
    scorecard::Scorecard sc;
    std::int64_t ts = kT0;
    // 0-89 min idle (cpu 10 %), 90-99 busy (50 %), 100-119 fullscreen game at low CPU, then one
    // sample of minute 120 closes minute 119.
    for (int minute = 0; minute <= 120; ++minute) {
        const int n = minute == 120 ? 1 : 30;
        for (int i = 0; i < n; ++i, ts += kTick) {
            const bool busy = minute >= 90 && minute < 100;
            const bool game = minute >= 100;
            telemetry::Sample s = plain(ts, busy ? 50.0 : 10.0,
                                        (busy ? 6 : (game ? 4 : 10)) * obs_test::kGBu);
            tr.append(s);
            sc.observe(s, game);
        }
    }
    const std::int64_t now = ts - kTick;
    const auto m = sc.compute(tr, {}, now);
    CHECK(near(m.idle_cpu_percent, 10.0));
    CHECK(near(m.idle_ram_used_bytes, 6.0 * static_cast<double>(obs_test::kGBu), 1.0));
    CHECK(near(m.disk_latency_ms, 4.0));
    CHECK(near(m.hard_faults_per_sec, 3.0));
    CHECK(!m.stutter_per_hour);  // no kernel data
    CHECK(!m.boot_ms);
    CHECK(!m.crashes_per_week);  // first start unknown
    CHECK(near(std::optional<double>(sc.hours_measured(tr, now)), 2.0));
    // Only the last 24 h count: a day later nothing is in the window.
    const auto later = sc.compute(tr, {}, now + kDay + 2 * 3600 * 1000);
    CHECK(!later.idle_cpu_percent && !later.hard_faults_per_sec && !later.disk_latency_ms);
}

TEST(scorecard_nulls_when_not_measured) {
    trace::Trace tr;
    scorecard::Scorecard sc;
    const auto m = sc.compute(tr, {}, kT0);
    CHECK(!m.any());
    const auto j = scorecard::metrics_to_json(m);
    for (const char* k : {"bootMs", "idleCpuPercent", "idleRamUsedBytes", "diskLatencyMs", "hardFaultsPerSec",
                          "stutterPerHour", "crashesPerWeek"})
        CHECK(j.find(k) && j.find(k)->is_null());
    CHECK_EQ(sc.hours_measured(tr, kT0), 0.0);
    // Fewer than 10 minutes of trace: still null.
    std::int64_t ts = kT0;
    for (int i = 0; i < 9 * 30 + 1; ++i, ts += kTick) tr.append(plain(ts, 5.0, 10 * obs_test::kGBu));
    const auto few = sc.compute(tr, {}, ts);
    CHECK(!few.idle_cpu_percent && !few.idle_ram_used_bytes && !few.hard_faults_per_sec);
    // A disk without I/O latency is not measured.
    trace::Trace quiet_disk;
    ts = kT0;
    for (int i = 0; i < 20 * 30 + 1; ++i, ts += kTick) {
        auto s = plain(ts, 5.0, 10 * obs_test::kGBu);
        s.disk_read_latency_ms = 0.0;
        s.disk_write_latency_ms = 0.0;
        quiet_disk.append(s);
    }
    const auto qd = sc.compute(quiet_disk, {}, ts);
    CHECK(!qd.disk_latency_ms);
    CHECK(qd.idle_cpu_percent.has_value());
}

TEST(scorecard_stutter_per_hour_from_kernel_minutes) {
    scorecard::Scorecard sc;
    trace::Trace tr;
    std::int64_t ts = (kT0 / kMin + 1) * kMin;  // wall-clock minute aligned
    for (int minute = 0; minute < 40; ++minute) {
        for (int i = 0; i < 30; ++i, ts += kTick) {
            telemetry::Sample s = plain(ts, 10.0, 10 * obs_test::kGBu);
            s.kernel_valid = true;
            s.kernel.running = true;
            s.kernel.worst_dpc_us = 300.0;
            s.kernel.worst_isr_us = 40.0;
            if ((minute == 7 || minute == 31) && i == 12) s.kernel.worst_dpc_us = 2500.0;
            if (minute == 20 && i == 3) s.kernel.worst_isr_us = 1999.0;  // below the threshold
            sc.observe(s, false);
        }
    }
    const auto m = sc.compute(tr, {}, ts);
    CHECK(near(m.stutter_per_hour, 2.0 * 60.0 / 40.0));
    // Fewer than 30 traced minutes -> null; samples without kernel data do not count.
    scorecard::Scorecard few;
    ts = kT0;
    for (int i = 0; i < 20 * 30; ++i, ts += kTick) {
        telemetry::Sample s = plain(ts, 10.0, 10 * obs_test::kGBu);
        s.kernel_valid = i < 20 * 30;
        s.kernel.running = true;
        few.observe(s, false);
    }
    for (int i = 0; i < 20 * 30; ++i, ts += kTick) few.observe(plain(ts, 10.0, 10 * obs_test::kGBu), false);
    CHECK(!few.compute(tr, {}, ts).stutter_per_hour);
}

TEST(scorecard_boot_and_crashes_from_events) {
    scorecard::Scorecard sc;
    trace::Trace tr;
    const std::int64_t now = kT0 + 10 * kDay;
    std::vector<telemetry::EventRecord> evs;
    // Six boots: the oldest (99 s) is outside the newest five.
    evs.push_back(ev(now - 9 * kDay, telemetry::EventKind::BootPerformance, 99000));
    evs.push_back(ev(now - 5 * kDay, telemetry::EventKind::BootPerformance, 30000));
    evs.push_back(ev(now - 4 * kDay, telemetry::EventKind::BootPerformance, 20000));
    evs.push_back(ev(now - 3 * kDay, telemetry::EventKind::BootPerformance, 40000));
    evs.push_back(ev(now - 2 * kDay, telemetry::EventKind::BootPerformance, 25000));
    evs.push_back(ev(now - 1 * kDay, telemetry::EventKind::BootPerformance, 35000));
    evs.push_back(ev(now - 1 * kDay, telemetry::EventKind::BootPerformance, 0));  // no value: ignored
    evs.push_back(ev(now - 1 * kDay, telemetry::EventKind::AppCrash));
    evs.push_back(ev(now - 2 * kDay, telemetry::EventKind::Bugcheck));
    evs.push_back(ev(now - 6 * kDay, telemetry::EventKind::AppCrash));  // before coverage
    evs.push_back(ev(now - 1 * kDay, telemetry::EventKind::AppHang));   // not a crash
    sc.set_first_start(now - 3 * kDay);  // + 24 h event-log backfill = 4 days covered
    const auto m = sc.compute(tr, evs, now);
    CHECK(near(m.boot_ms, 30000.0));
    CHECK(near(m.crashes_per_week, 2.0 * 7.0 / 4.0));
    // An even count of boots: mean of the middle two.
    std::vector<telemetry::EventRecord> two = {ev(now - 1000, telemetry::EventKind::BootPerformance, 10000),
                                               ev(now - 2000, telemetry::EventKind::BootPerformance, 20000)};
    CHECK(near(sc.compute(tr, two, now).boot_ms, 15000.0));
    // Covered for 7 days at most.
    sc.set_first_start(now - 30 * kDay);
    CHECK(near(sc.compute(tr, evs, now).crashes_per_week, 3.0));  // incl. the one 6 days ago
}

TEST(scorecard_persistence_round_trip_and_strict_parsing) {
    scorecard::Scorecard sc;
    sc.set_first_start(kT0);
    scorecard::Baseline b;
    b.captured_at = kT0 + kDay;
    b.metrics.boot_ms = 31000.0;
    b.metrics.idle_cpu_percent = 3.5;
    b.metrics.stutter_per_hour = 0.0;
    sc.set_baseline(b);
    telemetry::Sample s = plain(kT0, 10.0, 10 * obs_test::kGBu);
    s.kernel_valid = true;
    s.kernel.running = true;
    s.kernel.worst_dpc_us = 3000.0;
    sc.observe(s, true);
    const std::string text = sc.serialize();

    scorecard::Scorecard back;
    CHECK(back.deserialize(text));
    CHECK_EQ(back.first_start(), kT0);
    CHECK(back.baseline().has_value());
    CHECK(back.baseline() && back.baseline()->captured_at == kT0 + kDay);
    CHECK(back.baseline() && near(back.baseline()->metrics.boot_ms, 31000.0) &&
          near(back.baseline()->metrics.stutter_per_hour, 0.0) && !back.baseline()->metrics.disk_latency_ms);
    CHECK_EQ(back.minutes(), std::size_t{1});
    CHECK_EQ(back.serialize(), text);

    // Invalid files leave the state untouched.
    CHECK(!back.deserialize("{not json"));
    CHECK(!back.deserialize("{\"version\":2,\"firstStartMs\":1}"));
    CHECK(!back.deserialize("{\"version\":1,\"firstStartMs\":1,\"baseline\":{\"capturedAt\":5,\"metrics\":{}}}"));
    CHECK_EQ(back.first_start(), kT0);
    CHECK(back.baseline().has_value());

    auto metrics = [](const char* text) {
        auto p = json::parse(text);
        return p.ok() ? scorecard::metrics_from_json(*p.value) : std::nullopt;
    };
    const char* ok = "{\"bootMs\":1,\"idleCpuPercent\":null,\"idleRamUsedBytes\":null,\"diskLatencyMs\":null,"
                     "\"hardFaultsPerSec\":null,\"stutterPerHour\":null,\"crashesPerWeek\":0}";
    CHECK(metrics(ok).has_value());
    CHECK(!metrics("{\"bootMs\":1}"));
    CHECK(!metrics("{\"bootMs\":-1,\"idleCpuPercent\":null,\"idleRamUsedBytes\":null,\"diskLatencyMs\":null,"
                   "\"hardFaultsPerSec\":null,\"stutterPerHour\":null,\"crashesPerWeek\":0}"));
    CHECK(!metrics("{\"bootMs\":\"1\",\"idleCpuPercent\":null,\"idleRamUsedBytes\":null,\"diskLatencyMs\":null,"
                   "\"hardFaultsPerSec\":null,\"stutterPerHour\":null,\"crashesPerWeek\":0}"));

    // Load / save through FileIO.
    obs_test::MemFileIO io;
    CHECK(sc.save(io, "/sc.json"));
    scorecard::Scorecard loaded;
    const auto rep = loaded.load(io, "/sc.json");
    CHECK(rep.found && rep.loaded == 1 && rep.corrupt == 0);
    io.files["/bad.json"] = "garbage";
    CHECK_EQ(loaded.load(io, "/bad.json").corrupt, std::size_t{1});
    CHECK_EQ(loaded.first_start(), kT0);
}

TEST(engine_scorecard_first_start_auto_capture_and_manual_capture) {
    Rig r;
    CHECK(r.io.files.count("/data/scorecard.json") == 1);
    CHECK(r.io.files["/data/scorecard.json"].find("\"firstStartMs\":1780000000000") != std::string::npos);
    r.run(400, [](std::int64_t ts) { return quiet(ts); });  // ~13 min
    auto v = r.eng->scorecard();
    CHECK(!v.baseline);
    CHECK(v.current.idle_cpu_percent.has_value());
    CHECK(near(v.current.idle_cpu_percent, 12.0, 1e-4));
    CHECK(near(v.current.idle_ram_used_bytes, 7.0 * static_cast<double>(obs_test::kGBu), 1.0));
    CHECK(near(v.current.crashes_per_week, 0.0));  // 24 h covered by the event-log backfill
    CHECK(!v.current.boot_ms);
    CHECK(v.hours_measured > 0.15 && v.hours_measured < 0.3);

    // A restart keeps the first start.
    r.make();
    r.now += kTick;
    r.tick(quiet(r.now));
    CHECK(r.io.files["/data/scorecard.json"].find("\"firstStartMs\":1780000000000") != std::string::npos);

    // 24 h after the first start the baseline is captured once.
    r.tick(quiet(kT0 + kDay - kTick));
    CHECK(!r.eng->scorecard().baseline);
    r.tick(quiet(kT0 + kDay + kTick));
    v = r.eng->scorecard();
    CHECK(v.baseline.has_value());
    const std::int64_t at = v.baseline ? v.baseline->captured_at : 0;
    CHECK_EQ(at, kT0 + kDay + kTick);
    CHECK(r.io.files["/data/scorecard.json"].find("\"capturedAt\":" + std::to_string(at)) != std::string::npos);
    r.tick(quiet(kT0 + kDay + 2 * kTick));
    const engine::ScorecardView again = r.eng->scorecard();
    CHECK(again.baseline && again.baseline->captured_at == at);

    // Manual capture replaces it and survives a restart.
    r.now += 60'000;
    const auto cap = r.eng->capture_baseline();
    CHECK(cap.persisted);
    CHECK_EQ(cap.baseline.captured_at, r.now);
    r.make();
    CHECK(r.eng->scorecard().baseline && r.eng->scorecard().baseline->captured_at == cap.baseline.captured_at);
}
