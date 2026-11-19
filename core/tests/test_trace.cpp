#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <set>
#include <string>

#include "json.hpp"
#include "observe_test_util.hpp"
#include "test_framework.hpp"
#include "trace.hpp"

using namespace opensweep;
using namespace opensweep::trace;
using obs_test::MemFileIO;

namespace {

Sample simple(std::int64_t ts, double cpu) {
    Sample s;
    s.ts_ms = ts;
    s.cpu_percent = cpu;
    s.commit_total = 5;
    s.commit_limit = 10;
    s.handles = 1000;
    return s;
}

constexpr std::int64_t kT0 = 1'700'000'040'000;  // arbitrary epoch ms, not minute aligned

std::int64_t minute_start(std::int64_t t) { return t - (t % 60'000); }

}  // namespace

TEST(trace_metric_catalogue) {
    const auto ms = metrics();
    CHECK_EQ(ms.size(), kMetricCount);
    std::set<std::string> names;
    for (std::size_t i = 0; i < ms.size(); ++i) {
        CHECK(index(ms[i].metric) == i);
        CHECK(std::strlen(ms[i].unit) > 0);
        names.insert(ms[i].name);
        CHECK(metric_from_name(ms[i].name) == ms[i].metric);
    }
    CHECK_EQ(names.size(), kMetricCount);
    CHECK(!metric_from_name("nope").has_value());
    Sample s;
    s.commit_total = 30;
    s.commit_limit = 120;
    CHECK(std::fabs(metric_value(s, Metric::CommitPercent) - 25.0) < 1e-9);
    s.commit_limit = 0;
    CHECK(std::isnan(metric_value(s, Metric::CommitPercent)));
    s.disk_read_latency_ms = 7.5;
    CHECK_EQ(metric_value(s, Metric::DiskReadLatencyMs), 7.5);
    CHECK(range_from_string("24h") == Range::Hours24);
    CHECK(!range_from_string("1y").has_value());
    CHECK_EQ(std::string(to_string(Range::Days14)), std::string("14d"));
}

TEST(trace_tier0_bounded) {
    Trace t;
    for (int i = 0; i < 2500; ++i) t.append(simple(kT0 + i * 2000LL, i));
    CHECK_EQ(t.tier0().size(), kTier0Rows);
    CHECK_EQ(t.tier0().front().ts_ms, kT0 + (2500 - 900) * 2000LL);
    CHECK(t.latest() != nullptr);
    CHECK_EQ(t.latest()->ts_ms, kT0 + 2499 * 2000LL);
}

TEST(trace_minute_and_quarter_aggregation) {
    Trace t;
    const std::int64_t base = minute_start(kT0) - (minute_start(kT0) % (15 * 60'000));  // quarter-aligned
    // Minute 0: cpu 10, 20, 30 -> mean 20, max 30. Also one NaN value for commit (limit 0).
    Sample a = simple(base + 1000, 10);
    Sample b = simple(base + 3000, 20);
    Sample c = simple(base + 5000, 30);
    c.commit_limit = 0;  // commitPercent NaN for this sample, ignored in the mean
    t.append(a);
    t.append(b);
    t.append(c);
    CHECK_EQ(t.tier1().size(), 0u);  // minute still open
    t.append(simple(base + 60'000, 50));  // closes minute 0
    CHECK_EQ(t.tier1().size(), 1u);
    const auto& r = t.tier1().back();
    CHECK_EQ(r.ts_ms, base);
    CHECK_EQ(r.count, 3u);
    CHECK(std::fabs(r.mean[index(Metric::CpuPercent)] - 20.0f) < 1e-4);
    CHECK(std::fabs(r.max[index(Metric::CpuPercent)] - 30.0f) < 1e-4);
    CHECK(std::fabs(r.mean[index(Metric::CommitPercent)] - 50.0f) < 1e-4);
    // Fill to the next quarter: minutes 1..14 one sample each at cpu 50, then one in the next quarter.
    for (int m = 2; m < 15; ++m) t.append(simple(base + m * 60'000LL, 50));
    CHECK_EQ(t.tier2().size(), 0u);
    t.append(simple(base + 15 * 60'000LL, 0));  // closes minute 14 -> quarter still open
    CHECK_EQ(t.tier2().size(), 0u);
    t.append(simple(base + 16 * 60'000LL, 0));  // closes minute 15 -> folds into new quarter, closes old
    CHECK_EQ(t.tier2().size(), 1u);
    const auto& q = t.tier2().back();
    CHECK_EQ(q.ts_ms, base);
    CHECK_EQ(q.count, 3u + 14u);
    // Weighted mean: (10+20+30 + 14*50) / 17
    CHECK(std::fabs(q.mean[index(Metric::CpuPercent)] - static_cast<float>((60.0 + 700.0) / 17.0)) < 1e-3);
    CHECK(std::fabs(q.max[index(Metric::CpuPercent)] - 50.0f) < 1e-4);
}

TEST(trace_tiers_bounded_long_run) {
    Trace t;
    // One sample per minute for 15 days: tier1 caps at 1440, tier2 at 1344.
    const std::int64_t base = kT0;
    const int minutes = 15 * 24 * 60;
    for (int m = 0; m < minutes; ++m) t.append(simple(base + m * 60'000LL, m % 100));
    CHECK_EQ(t.tier0().size(), kTier0Rows);
    CHECK_EQ(t.tier1().size(), kTier1Rows);
    CHECK_EQ(t.tier2().size(), kTier2Rows);
    // History stays within the point limit for every range and metric.
    for (Range r : {Range::Minutes30, Range::Hours24, Range::Days14}) {
        for (const auto& mi : metrics()) {
            const auto h = t.history(mi.metric, r);
            CHECK(h.size() <= kMaxHistoryPoints);
        }
    }
    const auto day = t.history(Metric::CpuPercent, Range::Hours24);
    CHECK(day.size() >= 700 && day.size() <= kMaxHistoryPoints);
    for (std::size_t i = 1; i < day.size(); ++i) CHECK(day[i].ts_ms > day[i - 1].ts_ms);
    const auto fortnight = t.history(Metric::CpuPercent, Range::Days14, Agg::Max);
    CHECK(fortnight.size() >= 600 && fortnight.size() <= kMaxHistoryPoints);
    for (const auto& p : fortnight) CHECK(p.value <= 99.0 + 1e-6);
}

TEST(trace_history_30m_and_nan_skipped) {
    Trace t;
    for (int i = 0; i < 1000; ++i) {
        Sample s = simple(kT0 + i * 2000LL, i % 7);
        if (i % 10 == 0) s.commit_limit = 0;  // NaN commitPercent
        t.append(s);
    }
    const auto cpu = t.history(Metric::CpuPercent, Range::Minutes30);
    CHECK_EQ(cpu.size(), 900u);
    const auto commit = t.history(Metric::CommitPercent, Range::Minutes30);
    CHECK_EQ(commit.size(), 810u);
    for (const auto& p : commit) CHECK(std::isfinite(p.value));
    // Range filter: after a long gap only recent points are inside the 30-minute window.
    t.append(simple(kT0 + 1000 * 2000LL + 3'600'000, 5));
    CHECK_EQ(t.history(Metric::CpuPercent, Range::Minutes30).size(), 1u);
    // Open minute bucket appears as the last 24h point.
    const auto day = t.history(Metric::CpuPercent, Range::Hours24);
    CHECK(!day.empty());
    CHECK_EQ(day.back().value, 5.0);
}

TEST(trace_window_capture) {
    Trace t;
    for (int i = 0; i < 100; ++i) t.append(simple(kT0 + i * 2000LL, i));
    const auto w = t.window(kT0 + 20 * 2000LL, kT0 + 29 * 2000LL);
    CHECK_EQ(w.size(), 10u);
    CHECK_EQ(w.front().v[index(Metric::CpuPercent)], 20.0f);
    CHECK(t.window(0, 1).empty());
}

TEST(trace_events_bounded_newest_first) {
    Trace t;
    for (int i = 0; i < 2600; ++i) {
        EventRecord e;
        e.ts_ms = i;
        e.kind = EventKind::AppCrash;
        e.subject = "app" + std::to_string(i);
        t.add_event(e);
    }
    CHECK_EQ(t.event_count(), kMaxEvents);
    const auto ev = t.events(3);
    CHECK_EQ(ev.size(), 3u);
    CHECK_EQ(ev[0].ts_ms, 2599);
    CHECK_EQ(ev[2].ts_ms, 2597);
    const auto all = t.events();
    CHECK_EQ(all.size(), kMaxEvents);
    CHECK(!all.empty() && all.back().ts_ms == 600);
}

TEST(trace_crc32_known_vector) {
    CHECK_EQ(crc32("123456789"), 0xCBF43926u);
    CHECK_EQ(crc32(""), 0u);
    // Incremental == one shot.
    CHECK_EQ(crc32("6789", crc32("12345")), 0xCBF43926u);
}

TEST(trace_jsonl_seal_open) {
    json::Value o = json::Value::object();
    o.set("a", 1);
    o.set("b", "x\"y");
    const std::string line = jsonl_seal(o);
    CHECK(line.back() == '\n');
    auto back = jsonl_open(line);
    CHECK(back.has_value());
    CHECK(back && *back == o);
    // Empty object.
    auto e = jsonl_open(jsonl_seal(json::Value::object()));
    CHECK(e && e->is_object() && e->size() == 0);
    // Tampering is detected.
    std::string bad = line;
    CHECK(bad[5] == '1');
    bad[5] = '2';  // {"a":1 -> {"a":2
    CHECK(!jsonl_open(bad).has_value());
    std::string bad_crc = line;
    bad_crc[bad_crc.size() - 4] = bad_crc[bad_crc.size() - 4] == '0' ? '1' : '0';
    CHECK(!jsonl_open(bad_crc).has_value());
    CHECK(!jsonl_open("{\"a\":1}").has_value());
    CHECK(!jsonl_open("").has_value());
    CHECK(!jsonl_open("garbage that is long enough to look like a line").has_value());
    CHECK(!jsonl_open(line.substr(0, line.size() / 2)).has_value());
}

TEST(trace_event_json_roundtrip) {
    EventRecord e;
    e.ts_ms = 123456;
    e.kind = EventKind::ServiceFailure;
    e.provider = "Service Control Manager";
    e.event_id = 7031;
    e.level = 2;
    e.subject = "Spooler";
    e.message = "terminated unexpectedly";
    e.value = 1.5;
    const auto back = event_from_json(event_to_json(e));
    CHECK(back.has_value());
    CHECK(back && back->kind == e.kind && back->subject == e.subject && back->event_id == 7031 && back->level == 2 &&
          back->value == 1.5 && back->ts_ms == 123456);
    for (int k = 0; k <= static_cast<int>(EventKind::Other); ++k) {
        const auto kind = static_cast<EventKind>(k);
        CHECK(event_kind_from_string(telemetry::to_string(kind)) == kind);
    }
    json::Value bad = event_to_json(e);
    bad.set("level", 900);
    CHECK(!event_from_json(bad).has_value());
    bad = event_to_json(e);
    bad.set("ts", "yesterday");
    CHECK(!event_from_json(bad).has_value());
    CHECK(!event_from_json(json::Value(3)).has_value());
}

namespace {
Trace filled_trace() {
    Trace t;
    obs_test::NormalGen gen(7);
    for (int i = 0; i < 3 * 1800; ++i) t.append(gen.sample(kT0 + i * 2000LL));  // 3 hours
    return t;
}

bool same_tiers(const Trace& a, const Trace& b) {
    if (a.tier0().size() != b.tier0().size() || a.tier1().size() != b.tier1().size() ||
        a.tier2().size() != b.tier2().size())
        return false;
    for (std::size_t i = 0; i < a.tier0().size(); ++i) {
        if (a.tier0()[i].ts_ms != b.tier0()[i].ts_ms) return false;
        if (std::memcmp(a.tier0()[i].v.data(), b.tier0()[i].v.data(), sizeof(Values)) != 0) return false;
    }
    for (std::size_t i = 0; i < a.tier1().size(); ++i) {
        if (a.tier1()[i].ts_ms != b.tier1()[i].ts_ms || a.tier1()[i].count != b.tier1()[i].count) return false;
        if (std::memcmp(a.tier1()[i].mean.data(), b.tier1()[i].mean.data(), sizeof(Values)) != 0) return false;
        if (std::memcmp(a.tier1()[i].max.data(), b.tier1()[i].max.data(), sizeof(Values)) != 0) return false;
    }
    for (std::size_t i = 0; i < a.tier2().size(); ++i) {
        if (a.tier2()[i].ts_ms != b.tier2()[i].ts_ms) return false;
        if (std::memcmp(a.tier2()[i].mean.data(), b.tier2()[i].mean.data(), sizeof(Values)) != 0) return false;
    }
    return true;
}
}  // namespace

TEST(trace_persistence_roundtrip) {
    const Trace t = filled_trace();
    CHECK(t.tier2().size() >= 10u);
    const std::string bytes = t.serialize();
    Trace u;
    const auto rep = u.deserialize(bytes);
    CHECK(rep.found);
    CHECK_EQ(rep.corrupt, 0u);
    CHECK_EQ(rep.loaded, t.tier0().size() + t.tier1().size() + t.tier2().size());
    CHECK(same_tiers(t, u));
    // File helper through the FileIO interface.
    MemFileIO io;
    CHECK(t.save(io, "trace.bin"));
    Trace v;
    const auto rep2 = v.load(io, "trace.bin");
    CHECK(rep2.found && rep2.corrupt == 0);
    CHECK(same_tiers(t, v));
    // Missing file: not found, nothing loaded, no crash.
    Trace w;
    const auto rep3 = w.load(io, "missing.bin");
    CHECK(!rep3.found && rep3.loaded == 0);
    // Saving with a failing FileIO reports failure.
    io.fail_writes = true;
    CHECK(!t.save(io, "trace.bin"));
}

TEST(trace_persistence_corrupt_block_skipped) {
    const Trace t = filled_trace();
    std::string bytes = t.serialize();
    // Locate the tier-1 block (type 11) by walking the block headers.
    auto u32 = [&](std::size_t p) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[p])) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[p + 1])) << 8) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[p + 2])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[p + 3])) << 24);
    };
    std::size_t pos = 20;
    std::size_t tier1_payload = 0;
    while (pos + 16 <= bytes.size()) {
        if (u32(pos) == 11) tier1_payload = pos + 16;
        pos += 16 + u32(pos + 8);
    }
    CHECK(tier1_payload > 0);
    bytes[tier1_payload + 100] = static_cast<char>(bytes[tier1_payload + 100] ^ 0x5A);
    Trace u;
    const auto rep = u.deserialize(bytes);
    CHECK_EQ(rep.corrupt, 1u);
    CHECK_EQ(u.tier1().size(), 0u);
    CHECK_EQ(u.tier0().size(), t.tier0().size());
    CHECK_EQ(u.tier2().size(), t.tier2().size());
}

TEST(trace_persistence_truncation_and_garbage_never_crash) {
    const Trace t = filled_trace();
    const std::string bytes = t.serialize();
    const std::size_t total = t.tier0().size() + t.tier1().size() + t.tier2().size();
    // Every truncation point near block boundaries + a stride through the file.
    for (std::size_t len = 0; len < bytes.size(); len += (len < 200 ? 1 : 997)) {
        Trace u;
        const auto rep = u.deserialize(std::string_view(bytes).substr(0, len));
        CHECK(rep.loaded <= total);
        CHECK(rep.corrupt >= 1u);  // truncated files always report damage
        CHECK(u.tier0().size() <= kTier0Rows && u.tier1().size() <= kTier1Rows && u.tier2().size() <= kTier2Rows);
    }
    // Header damage: nothing is loaded.
    std::string hdr = bytes;
    hdr[0] = 'X';
    Trace u;
    auto rep = u.deserialize(hdr);
    CHECK_EQ(rep.loaded, 0u);
    CHECK_EQ(rep.corrupt, 1u);
    // Random garbage and random bit flips.
    std::mt19937 rng(1);
    for (int i = 0; i < 200; ++i) {
        std::string g = bytes;
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int f = 0; f < flips; ++f) {
            const std::size_t k = rng() % g.size();
            g[k] = static_cast<char>(g[k] ^ static_cast<char>(1 + rng() % 255));
        }
        Trace v;
        const auto r = v.deserialize(g);
        CHECK(r.loaded <= total);
        CHECK(v.tier0().size() <= kTier0Rows);
    }
    std::string noise(5000, '\0');
    for (auto& ch : noise) ch = static_cast<char>(rng());
    Trace w;
    CHECK_EQ(w.deserialize(noise).loaded, 0u);
    // Appended garbage after a valid file is reported but valid data is kept.
    Trace x;
    rep = x.deserialize(bytes + "trailing");
    CHECK_EQ(rep.loaded, total);
    CHECK_EQ(rep.corrupt, 1u);
}

TEST(trace_events_jsonl_persist_load_compact) {
    MemFileIO io;
    Trace t;
    auto ev = [](int i) {
        EventRecord e;
        e.ts_ms = 1000 + i;
        e.kind = i % 2 ? EventKind::AppCrash : EventKind::DiskError;
        e.provider = "p";
        e.subject = "s" + std::to_string(i);
        e.message = "m";
        return e;
    };
    for (int i = 0; i < 10; ++i) t.add_event(ev(i));
    CHECK(t.persist_events(io, "events.jsonl"));  // first persist: full atomic write
    CHECK_EQ(io.atomic_writes, 1);
    for (int i = 10; i < 15; ++i) t.add_event(ev(i));
    CHECK(t.persist_events(io, "events.jsonl"));  // append
    CHECK_EQ(io.appends, 1);
    CHECK_EQ(obs_test::count_lines(io.files["events.jsonl"]), 15u);
    CHECK(t.persist_events(io, "events.jsonl"));  // nothing new
    CHECK_EQ(io.appends, 1);

    Trace u;
    auto rep = u.load_events(io, "events.jsonl");
    CHECK(rep.found && rep.loaded == 15u && rep.corrupt == 0u);
    const auto newest = u.events(1);
    CHECK(!newest.empty() && newest[0].subject == "s14");

    // Corrupt one line and append a torn (partial) line: both are skipped and counted.
    std::string& f = io.files["events.jsonl"];
    const auto p = f.find("s3");
    f[p + 1] = '9';
    f += "{\"ts\":1,\"kind\":\"app_cr";
    Trace w;
    rep = w.load_events(io, "events.jsonl");
    CHECK_EQ(rep.loaded, 14u);
    CHECK_EQ(rep.corrupt, 2u);

    // Growth beyond 2x capacity triggers an atomic compaction; the file stays bounded.
    MemFileIO io2;
    Trace big;
    for (int i = 0; i < 9000; ++i) {
        big.add_event(ev(i));
        if (i % 250 == 0) CHECK(big.persist_events(io2, "e.jsonl"));
    }
    CHECK(big.persist_events(io2, "e.jsonl"));
    CHECK(obs_test::count_lines(io2.files["e.jsonl"]) <= 2 * kMaxEvents);
    CHECK(io2.atomic_writes >= 2);
    Trace back;
    rep = back.load_events(io2, "e.jsonl");
    CHECK_EQ(back.event_count(), kMaxEvents);
    const auto newest_big = back.events(1);
    CHECK(!newest_big.empty() && newest_big[0].ts_ms == 1000 + 8999);

    // A failed write keeps events pending for the next attempt.
    MemFileIO io3;
    Trace t3;
    t3.add_event(ev(1));
    io3.fail_writes = true;
    CHECK(!t3.persist_events(io3, "x.jsonl"));
    io3.fail_writes = false;
    CHECK(t3.persist_events(io3, "x.jsonl"));
    CHECK_EQ(obs_test::count_lines(io3.files["x.jsonl"]), 1u);
}

TEST(trace_std_file_io) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("opensweep_trace_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    const std::string path = (dir / "trace.bin").string();
    StdFileIO io;
    const Trace t = filled_trace();
    CHECK(t.save(io, path));
    CHECK(fs::exists(path));
    CHECK(!fs::exists(path + ".tmp"));
    CHECK(t.save(io, path));  // overwrite existing target
    Trace u;
    const auto rep = u.load(io, path);
    CHECK(rep.found && rep.corrupt == 0);
    CHECK(same_tiers(t, u));
    std::string small;
    CHECK(!io.read_all(path, small, 10));  // size limit enforced
    const std::string ep = (dir / "events.jsonl").string();
    CHECK(io.append(ep, "a\n"));
    CHECK(io.append(ep, "b\n"));
    std::string text;
    CHECK(io.read_all(ep, text, 100));
    CHECK_EQ(text, std::string("a\nb\n"));
    CHECK(!io.read_all((dir / "missing").string(), text, 100));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
