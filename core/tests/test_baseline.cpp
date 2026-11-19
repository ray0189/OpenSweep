#include <cmath>
#include <limits>
#include <string>

#include "baseline.hpp"
#include "json.hpp"
#include "observe_test_util.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using namespace opensweep::baseline;
using obs_test::MemFileIO;

namespace {
constexpr std::int64_t kT0 = 1'700'000'000'000;  // minute aligned
bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
}  // namespace

TEST(baseline_single_stats_and_readiness) {
    Baseline b(BaselineConfig{100, 5, 0.5, 0.1});
    CHECK(!b.ready());
    CHECK(std::isnan(b.median()));
    CHECK(!b.z(1.0, 1.0).has_value());
    for (double v : {1.0, 1.0, 2.0, 2.0}) b.add(v);
    CHECK(!b.ready());
    b.add(std::numeric_limits<double>::quiet_NaN());  // ignored
    b.add(std::numeric_limits<double>::infinity());   // ignored
    CHECK_EQ(b.samples(), 4u);
    for (double v : {4.0, 6.0, 9.0}) b.add(v);
    CHECK(b.ready());
    CHECK(near(b.median(), 2.0));
    CHECK(near(b.mad(), 1.0));
    CHECK(near(*b.z(4.0, 0.0), 0.6745 * 2.0));
    CHECK(near(*b.z(4.0, 4.0), 0.6745 * 0.5));  // MAD floor
    CHECK(b.ewma_fast() > b.ewma_slow());       // recent values are higher
}

TEST(baseline_window_bounded) {
    Baseline b(BaselineConfig{10, 3, 0.3, 0.02});
    for (int i = 0; i < 1000; ++i) b.add(i);
    CHECK_EQ(b.window_size(), 10u);
    CHECK_EQ(b.samples(), 1000u);
    CHECK(near(b.median(), 994.5));
}

TEST(baseline_set_minute_aggregation_and_ready) {
    BaselineSetConfig cfg;
    cfg.overall.ready_after = 120;
    BaselineSet set(cfg);
    obs_test::NormalGen gen(3);
    // 119 complete minutes + the first sample of minute 120: not ready yet (119 values committed).
    for (int i = 0; i < 119 * 30 + 1; ++i) set.update(gen.sample(kT0 + i * 2000LL));
    CHECK_EQ(set.get(Metric::CpuPercent).samples(), 119u);
    CHECK(!set.get(Metric::CpuPercent).ready());
    for (int i = 119 * 30 + 1; i < 121 * 30 + 1; ++i) set.update(gen.sample(kT0 + i * 2000LL));
    CHECK(set.get(Metric::CpuPercent).ready());
    const auto& c = set.get(Metric::CommitPercent);
    CHECK(near(c.median(), 50.0, 0.5));
    CHECK(c.mad() < 0.5);
    // Minute means are much less noisy than single samples: cpu median close to the uniform mean 16.5.
    CHECK(near(set.get(Metric::CpuPercent).median(), 16.5, 2.0));
}

TEST(baseline_set_active_idle_split) {
    BaselineSet set;
    // 10 minutes busy (cpu 60) then 10 minutes idle (cpu 2).
    std::int64_t ts = kT0;
    for (int m = 0; m < 20; ++m) {
        for (int i = 0; i < 30; ++i, ts += 2000) {
            telemetry::Sample s;
            s.ts_ms = ts;
            s.cpu_percent = m < 10 ? 60.0 : 2.0;
            s.cpu_freq_percent = m < 10 ? 110.0 : 60.0;
            set.update(s);
        }
    }
    telemetry::Sample last;
    last.ts_ms = ts;
    set.update(last);
    CHECK_EQ(set.get(Metric::CpuPercent, Context::Active).samples(), 10u);
    CHECK_EQ(set.get(Metric::CpuPercent, Context::Idle).samples(), 10u);
    CHECK_EQ(set.get(Metric::CpuPercent).samples(), 20u);
    CHECK(near(set.get(Metric::CpuFreqPercent, Context::Active).median(), 110.0));
    CHECK(near(set.get(Metric::CpuFreqPercent, Context::Idle).median(), 60.0));
}

TEST(baseline_boot_events) {
    BaselineSet set;
    std::vector<telemetry::EventRecord> evs(3);
    for (std::size_t i = 0; i < evs.size(); ++i) {
        evs[i].kind = telemetry::EventKind::BootPerformance;
        evs[i].value = 30000.0 + static_cast<double>(i) * 1000.0;
    }
    telemetry::EventRecord other;
    other.kind = telemetry::EventKind::AppCrash;
    other.value = 99999;
    evs.push_back(other);
    set.observe_events(evs);
    CHECK_EQ(set.boot().samples(), 3u);
    CHECK(set.boot().ready());
    CHECK(near(set.boot().median(), 31000.0));
}

TEST(baseline_persistence_roundtrip) {
    BaselineSetConfig cfg;
    cfg.overall.ready_after = 30;
    BaselineSet set(cfg);
    obs_test::NormalGen gen(9);
    for (int i = 0; i < 40 * 30 + 1; ++i) set.update(gen.sample(kT0 + i * 2000LL));
    set.boot_mut().add(25000);
    MemFileIO io;
    CHECK(set.save(io, "baselines.json"));
    BaselineSet back(cfg);
    const auto rep = back.load(io, "baselines.json");
    CHECK(rep.found);
    CHECK_EQ(rep.corrupt, 0u);
    CHECK_EQ(rep.loaded, trace::kMetricCount);
    for (const auto& mi : trace::metrics()) {
        for (Context c : {Context::All, Context::Active, Context::Idle}) {
            const auto& a = set.get(mi.metric, c);
            const auto& b = back.get(mi.metric, c);
            CHECK_EQ(a.samples(), b.samples());
            CHECK_EQ(a.ready(), b.ready());
            CHECK_EQ(a.window_size(), b.window_size());
            if (a.window_size() > 0) {
                CHECK(near(a.median(), b.median(), 1e-9 + std::fabs(a.median()) * 1e-12));
                CHECK(near(a.ewma_slow(), b.ewma_slow(), std::fabs(a.ewma_slow()) * 1e-12));
            }
        }
    }
    CHECK_EQ(back.boot().samples(), 1u);
    // summary shape for IPC
    const auto sum = back.summary_json();
    const auto* m = sum.find("metrics");
    CHECK(m && m->find("cpuPercent") && m->find("cpuPercent")->find("ready") &&
          m->find("cpuPercent")->find("ready")->as_bool());
    CHECK(m && m->find("bootDurationMs"));
}

TEST(baseline_persistence_rejects_garbage) {
    BaselineSet set;
    obs_test::NormalGen gen(1);
    for (int i = 0; i < 10 * 30 + 1; ++i) set.update(gen.sample(kT0 + i * 2000LL));
    const json::Value good = set.to_json();

    // Wrong version / shape: everything rejected, set left empty.
    {
        json::Value v = good;
        v.set("version", 99);
        BaselineSet b;
        CHECK(b.from_json(v) >= 1u);
        CHECK_EQ(b.get(Metric::CpuPercent).samples(), 0u);
        CHECK(b.from_json(json::Value("str")) >= 1u);
        CHECK(b.from_json(json::Value::object()) >= 1u);
    }
    auto mutate_cpu_all = [&](const char* field, json::Value val) {
        json::Value v = good;
        json::Value metrics = *v.find("metrics");
        json::Value cpu = *metrics.find("cpuPercent");
        json::Value all = *cpu.find("all");
        all.set(field, std::move(val));
        cpu.set("all", std::move(all));
        metrics.set("cpuPercent", std::move(cpu));
        v.set("metrics", std::move(metrics));
        return v;
    };
    // Bad fields reject only that baseline; the rest is restored.
    for (auto v : {mutate_cpu_all("samples", json::Value(-5)), mutate_cpu_all("samples", json::Value("ten")),
                   mutate_cpu_all("ewmaFast", json::Value("x")), mutate_cpu_all("window", json::Value(3)),
                   mutate_cpu_all("window", json::Value(json::Array{json::Value(1.0), json::Value("2")})),
                   mutate_cpu_all("window", json::Value(json::Array{json::Value(1e300)})),  // overflows float
                   mutate_cpu_all("samples", json::Value(0))}) {                           // window > samples
        BaselineSet b;
        CHECK_EQ(b.from_json(v), 1u);
        CHECK_EQ(b.get(Metric::CpuPercent).samples(), 0u);
        CHECK(b.get(Metric::MemLoadPercent).samples() > 0u);
    }
    // NaN / Infinity / overflowing numbers cannot even be parsed (strict JSON) -> file rejected.
    MemFileIO io;
    for (const char* text : {"{\"version\":1,\"metrics\":{\"cpuPercent\":{\"all\":{\"samples\":1,\"ewmaFast\":NaN}}}}",
                             "{\"version\":1,\"metrics\":{},\"boot\":{\"samples\":1e999}}", "not json at all", ""}) {
        io.files["b.json"] = text;
        BaselineSet b;
        const auto rep = b.load(io, "b.json");
        CHECK(rep.found);
        CHECK(rep.corrupt >= 1u);
        CHECK_EQ(rep.loaded, 0u);
    }
    // Unknown metrics are ignored, not counted as corrupt.
    {
        json::Value v = good;
        json::Value metrics = *v.find("metrics");
        metrics.set("someFutureMetric", json::Value::object());
        v.set("metrics", std::move(metrics));
        BaselineSet b;
        CHECK_EQ(b.from_json(v), 0u);
    }
}
