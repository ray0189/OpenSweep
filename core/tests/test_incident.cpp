#include <cmath>
#include <set>
#include <string>

#include "incident.hpp"
#include "json.hpp"
#include "observe_test_util.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using namespace opensweep::incident;
using obs_test::MemFileIO;

namespace {

Incident sample_incident(const std::string& key = "storage_latency") {
    Incident inc;
    inc.detector = "storage_latency";
    inc.key = key;
    inc.title = "Storage latency spike";
    inc.summary = "Read latency reached 120 ms; hard faults of app.exe are correlated with this period.";
    inc.symptoms = {"Applications pause"};
    Evidence e;
    e.metric = "diskReadLatencyMs";
    e.unit = "ms";
    e.observed = 120;
    e.baseline = 2.0;
    e.robust_z = 40.0;
    e.window_from_ms = 1000;
    e.window_to_ms = 2000;
    e.note = "floor 50 ms";
    inc.evidence.push_back(e);
    Evidence e2;
    e2.metric = "diskQueueLength";
    e2.observed = 3;  // baseline and z stay NaN -> null
    inc.evidence.push_back(e2);
    inc.contributors.push_back({"app.exe", "process", 42, 0.8});
    inc.confidence = 0.55;
    inc.confidence_level = Level::Medium;
    inc.risk = Level::Low;
    inc.severity = Severity::Warning;
    inc.recommended_actions = {"Check Resource Monitor."};
    return inc;
}

}  // namespace

TEST(incident_confidence_model) {
    ConfidenceInputs base{0.5, 60'000, 60'000, 1, true};
    const double c0 = compute_confidence(base);
    CHECK(c0 > 0.05 && c0 < 0.99);
    ConfidenceInputs stronger = base;
    stronger.strength = 1.0;
    ConfidenceInputs longer = base;
    longer.duration_ms = 120'000;
    ConfidenceInputs more = base;
    more.agreeing_signals = 3;
    ConfidenceInputs not_ready = base;
    not_ready.baseline_ready = false;
    CHECK(compute_confidence(stronger) > c0);
    CHECK(compute_confidence(longer) > c0);
    CHECK(compute_confidence(more) > c0);
    CHECK(compute_confidence(not_ready) < c0);
    CHECK(confidence_level(compute_confidence(not_ready)) == Level::Low);
    // Even maximal evidence stays "low" without a baseline.
    CHECK(confidence_level(compute_confidence({1.0, 1e9, 1.0, 10, false})) == Level::Low);
    CHECK(confidence_level(compute_confidence({1.0, 1e9, 1.0, 10, true})) == Level::High);
    CHECK(confidence_level(compute_confidence({0.0, 0.0, 60'000, 0, true})) == Level::Low);
    // Bounds and garbage.
    const double nan = std::nan("");
    const double cg = compute_confidence({nan, nan, 1.0, -5, true});
    CHECK(cg >= 0.05 && cg <= 0.99);
    CHECK(confidence_level(nan) == Level::Low);
    CHECK(confidence_level(0.39) == Level::Low);
    CHECK(confidence_level(0.4) == Level::Medium);
    CHECK(confidence_level(0.7) == Level::High);
    CHECK(z_strength(3.0) == 0.0);
    CHECK(z_strength(9.0) == 1.0);
    CHECK(z_strength(-6.0) == 0.5);
    CHECK(z_strength(nan) == 0.0);
}

TEST(incident_wording_guard) {
    Incident inc = sample_incident();
    CHECK(wording_ok(inc));
    inc.summary = "Latency was Caused By app.exe";
    CHECK(!wording_ok(inc));
    inc = sample_incident();
    inc.recommended_actions.push_back("This is due to the driver");
    CHECK(!wording_ok(inc));
    inc = sample_incident();
    inc.evidence[0].note = "because of x";
    CHECK(!wording_ok(inc));
}

TEST(incident_json_roundtrip_and_validation) {
    Incident inc = sample_incident();
    inc.id = "inc-5-1";
    inc.first_seen_ms = 5;
    inc.last_seen_ms = 9;
    inc.occurrences = 3;
    const json::Value v = to_json(inc);
    // Through text, as on disk.
    const auto parsed = json::parse(json::serialize(v));
    CHECK(parsed.ok());
    const auto back = from_json(*parsed.value);
    CHECK(back.has_value());
    if (back) {
        CHECK_EQ(back->id, inc.id);
        CHECK_EQ(back->title, inc.title);
        CHECK_EQ(back->occurrences, 3u);
        CHECK_EQ(back->evidence.size(), 2u);
        CHECK_EQ(back->evidence[0].robust_z, 40.0);
        CHECK(std::isnan(back->evidence[1].baseline));
        CHECK_EQ(back->contributors[0].pid, 42u);
        CHECK(back->risk == Level::Low && back->severity == Severity::Warning && back->state == State::Open);
        CHECK(!back->auto_recovery_allowed);
        CHECK(json::serialize(to_json(*back)) == json::serialize(v));
    }
    auto mutated = [&](const char* key, json::Value val) {
        json::Value m = v;
        m.set(key, std::move(val));
        return from_json(m);
    };
    CHECK(!mutated("confidence", json::Value(1.5)).has_value());
    CHECK(!mutated("severity", json::Value("apocalyptic")).has_value());
    CHECK(!mutated("state", json::Value(1)).has_value());
    CHECK(!mutated("id", json::Value("")).has_value());
    CHECK(!mutated("autoRecoveryAllowed", json::Value("yes")).has_value());
    CHECK(!mutated("occurrences", json::Value(0)).has_value());
    CHECK(!mutated("evidence", json::Value(json::Array{json::Value(1)})).has_value());
    CHECK(!mutated("title", json::Value(std::string(5000, 'x'))).has_value());
    CHECK(!from_json(json::Value::array()).has_value());
}

TEST(incident_store_merge_resolve_reopen) {
    IncidentStore st;
    auto up = st.open_or_merge(sample_incident(), 1000);
    CHECK(up.created && up.incident);
    const std::string id = up.incident->id;
    CHECK(id.rfind("inc-1000-", 0) == 0);
    CHECK(st.find_open("storage_latency") != nullptr);
    // Repeat while open: merged, counted (event-style).
    up = st.open_or_merge(sample_incident(), 2000);
    CHECK(!up.created && up.incident->id == id);
    CHECK_EQ(up.incident->occurrences, 2u);
    CHECK_EQ(up.incident->first_seen_ms, 1000);
    CHECK_EQ(up.incident->last_seen_ms, 2000);
    // Refresh without counting (level detectors).
    up = st.open_or_merge(sample_incident(), 2500, IncidentStore::kDefaultMergeWindowMs, false);
    CHECK_EQ(up.incident->occurrences, 2u);
    CHECK(st.touch("storage_latency", 3000, 500.0));
    CHECK_EQ(st.find_open("storage_latency")->evidence[0].observed, 500.0);  // peak kept
    CHECK(st.touch("storage_latency", 3100, 100.0));
    CHECK_EQ(st.find_open("storage_latency")->evidence[0].observed, 500.0);
    CHECK(!st.touch("nope", 3000));
    // Resolve, then a repeat within the merge window re-opens the same incident.
    CHECK(st.resolve("storage_latency", 4000));
    CHECK(st.find_open("storage_latency") == nullptr);
    CHECK(!st.resolve("storage_latency", 4001));
    CHECK(!st.touch("storage_latency", 4002));
    up = st.open_or_merge(sample_incident(), 4000 + 60'000);
    CHECK(up.reopened && !up.created && up.incident->id == id);
    CHECK_EQ(up.incident->occurrences, 3u);
    CHECK(up.incident->state == State::Open && up.incident->resolved_ms == 0);
    // Resolve and repeat long after: a new incident.
    st.resolve("storage_latency", 100'000);
    up = st.open_or_merge(sample_incident(), 100'000 + IncidentStore::kDefaultMergeWindowMs + 1);
    CHECK(up.created && up.incident->id != id);
    CHECK_EQ(st.size(), 2u);
    CHECK_EQ(st.open_count(), 1u);
    const auto list = st.list();
    CHECK_EQ(list.size(), 2u);
    CHECK(list[0].last_seen_ms >= list[1].last_seen_ms);
    CHECK(st.find_id(id) != nullptr);
}

TEST(incident_store_bounded_and_ids_unique) {
    IncidentStore st(500);
    std::set<std::string> ids;
    for (int i = 0; i < 1200; ++i) {
        auto up = st.open_or_merge(sample_incident("k" + std::to_string(i)), 1000 + i);
        ids.insert(up.incident->id);
        if (i % 3 != 0) st.resolve("k" + std::to_string(i), 1000 + i);
        CHECK(st.size() <= 500u);
    }
    CHECK_EQ(ids.size(), 1200u);
    CHECK_EQ(st.size(), 500u);
    // Open incidents are evicted last: all 400 open ones from i%3==0 within the last ... remain if possible.
    CHECK(st.open_count() == 400u);
    const auto top10 = st.list(10);
    CHECK_EQ(top10.size(), 10u);
    CHECK(!top10.empty() && top10[0].key == "k1199");
}

TEST(incident_store_persistence) {
    MemFileIO io;
    IncidentStore st;
    st.open_or_merge(sample_incident("a"), 1000);
    st.open_or_merge(sample_incident("b"), 2000);
    CHECK(st.persist(io, "incidents.jsonl"));
    CHECK_EQ(io.atomic_writes, 1);
    CHECK_EQ(obs_test::count_lines(io.files["incidents.jsonl"]), 2u);
    st.resolve("a", 3000);
    st.open_or_merge(sample_incident("c"), 4000);
    CHECK(st.persist(io, "incidents.jsonl"));  // appends the two changed ones
    CHECK_EQ(io.appends, 1);
    CHECK_EQ(obs_test::count_lines(io.files["incidents.jsonl"]), 4u);
    CHECK(st.persist(io, "incidents.jsonl"));  // nothing dirty
    CHECK_EQ(io.appends, 1);

    IncidentStore back;
    auto rep = back.load(io, "incidents.jsonl");
    CHECK(rep.found && rep.corrupt == 0u);
    CHECK_EQ(rep.loaded, 3u);
    CHECK_EQ(back.size(), 3u);
    CHECK(back.find_open("a") == nullptr);  // last line per id wins (resolved)
    CHECK(back.find_open("b") != nullptr);
    // Sequence continues after the highest loaded id.
    auto up = back.open_or_merge(sample_incident("d"), 5000);
    CHECK(up.incident->id == "inc-5000-4");

    // Corrupt line + torn tail: skipped and counted, others kept.
    std::string& f = io.files["incidents.jsonl"];
    const auto p = f.find("Storage latency");
    f[p] = 's';
    f += "{\"id\":\"inc-9";
    IncidentStore damaged;
    rep = damaged.load(io, "incidents.jsonl");
    CHECK_EQ(rep.corrupt, 2u);
    CHECK(damaged.size() >= 2u);

    // Compaction keeps the file bounded.
    MemFileIO io2;
    IncidentStore big(50);
    for (int i = 0; i < 1000; ++i) {
        big.open_or_merge(sample_incident("k" + std::to_string(i % 70)), 1000 + i * 1000LL);
        if (i % 7 == 0) big.resolve("k" + std::to_string((i + 3) % 70), 1000 + i * 1000LL);
        CHECK(big.persist(io2, "i.jsonl"));
        CHECK(obs_test::count_lines(io2.files["i.jsonl"]) <= 2 * 50u + 2u);
    }
    IncidentStore big_back(50);
    rep = big_back.load(io2, "i.jsonl");
    CHECK_EQ(rep.corrupt, 0u);
    CHECK(big_back.size() <= 50u);
    // Missing file.
    IncidentStore none;
    rep = none.load(io, "missing.jsonl");
    CHECK(!rep.found);
}
