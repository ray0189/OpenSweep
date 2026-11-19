#include <algorithm>
#include <string>
#include <vector>

#include "commands.hpp"
#include "commands2.hpp"
#include "engine_test_util.hpp"
#include "framing.hpp"
#include "protocol.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using namespace eng_test;
using protocol::Capability;
using protocol::ClientContext;

namespace {

struct FakeProvider : commands::SystemProvider {
    bool metrics(commands::MetricsSnapshot& out) override {
        out.timestamp_ms = kT0;
        out.memory.total_phys = 16 * obs_test::kGBu;
        return true;
    }
    bool topology(commands::TopologyInfo& out) override {
        out.logical_processors = 8;
        out.cores = 4;
        out.packages = 1;
        out.numa_nodes = 1;
        out.groups = 1;
        out.efficiency_classes = {4};
        out.l3_caches = 1;
        return true;
    }
    bool self_process(commands::SelfProcessInfo& out) override {
        out.working_set = 1;
        out.private_bytes = 1;
        return true;
    }
    std::uint32_t pid() override { return 4242; }
};

struct Env {
    Rig rig;
    FakeProvider provider;
    commands::ServiceInfo info;
    commands::ServerCounters counters;
    commands2::Platform platform;
    protocol::Dispatcher d;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> fg_calls;

    Env() {
        info.started_at_ms = kT0;
        platform.provider = &provider;
        platform.service = &info;
        for (std::uint32_t i = 0; i < 8; ++i) platform.cpu_sets.push_back({256 + i, 0, i / 2, 0, 0});
        platform.report_foreground = [this](std::uint32_t pid, std::uint32_t session) {
            fg_calls.emplace_back(pid, session);
            if (pid == 100 || pid == 200) return session == 1 ? commands2::ForegroundVerdict::Accepted
                                                              : commands2::ForegroundVerdict::WrongSession;
            if (pid == 300) return commands2::ForegroundVerdict::Ignored;
            return commands2::ForegroundVerdict::NoSuchProcess;
        };
        commands::register_phase1(d, provider, info, counters, commands2::engine_mode_provider(*rig.eng));
        commands2::register_engine_commands(d, *rig.eng, platform);
    }

    json::Value call(const std::string& cmd, const std::string& args = "", bool admin = false,
                     std::uint32_t session = 1, std::size_t* bytes = nullptr) {
        ClientContext ctx;
        ctx.pid = 77;
        ctx.session_id = session;
        ctx.capabilities = protocol::cap_bit(Capability::Read) | (admin ? protocol::cap_bit(Capability::Admin) : 0u);
        std::string req = "{\"v\":1,\"id\":9,\"cmd\":\"" + cmd + "\"";
        if (!args.empty()) req += ",\"args\":" + args;
        req += "}";
        const auto out = d.handle(req, ctx, framing::kMaxResponsePayload);
        if (bytes) *bytes = out.response.size();
        auto parsed = json::parse(out.response);
        return parsed.ok() ? *parsed.value : json::Value();
    }
    static bool ok(const json::Value& r) { return r.find("ok") && r.find("ok")->as_bool(); }
    static std::string code(const json::Value& r) {
        const auto* e = r.find("error");
        const auto* c = e ? e->find("code") : nullptr;
        return c ? c->as_string() : std::string();
    }
    static const json::Value& res(const json::Value& r) {
        static const json::Value empty;
        const auto* x = r.find("result");
        return x ? *x : empty;
    }
};

bool is_num(const json::Value* v) { return v && v->is_number(); }
bool is_int(const json::Value* v) { return v && v->is_int(); }

}  // namespace

TEST(cmd2_every_contract_command_is_registered_with_the_right_capability) {
    Env e;
    for (const char* c : {"status", "sample", "history", "baselines", "incidents", "events", "recommendations",
                          "policies", "journal", "heal", "topology", "reportForeground", "setMode", "rollbackAll",
                          "disablePolicy", "approve", "exportDiagnostics", "hello", "health", "metrics"})
        CHECK(e.d.has(c));
    // Admin commands: forbidden for read-only clients, before any argument validation.
    for (const char* c : {"setMode", "rollbackAll", "disablePolicy", "approve", "exportDiagnostics"}) {
        const auto r = e.call(c, "{\"bogus\":1}");
        CHECK_EQ(Env::code(r), std::string("forbidden"));
    }
    CHECK(e.rig.io.files.count("/data/diagnostics.json") == 0);
}

TEST(cmd2_status_shape_and_hello_mode) {
    Env e;
    e.rig.run(3, [](std::int64_t ts) { return quiet(ts); });
    const auto r = e.call("status");
    CHECK(Env::ok(r));
    const auto& s = Env::res(r);
    CHECK_EQ(s.find("mode")->as_string(), std::string("monitor"));
    CHECK(s.find("simulation")->as_bool());
    const auto* eng = s.find("engine");
    CHECK(eng && eng->find("running")->is_bool() && is_int(eng->find("ticks")) && is_num(eng->find("lastTickMs")) &&
          is_num(eng->find("tickCostUs")));
    CHECK_EQ(eng->find("ticks")->as_int().value_or(0), 3);
    CHECK(is_num(s.find("stabilityScore")) && s.find("stabilityScore")->as_double() <= 100);
    CHECK(s.find("scoreReasons")->is_array());
    const auto* counts = s.find("counts");
    CHECK(counts && is_int(counts->find("incidentsOpen")) && is_int(counts->find("policiesActive")) &&
          is_int(counts->find("recommendations")));
    CHECK(s.find("heal") && s.find("heal")->find("breakerOpen")->is_bool());
    CHECK(Env::code(e.call("status", "{\"x\":1}")) == "bad_args");

    CHECK_EQ(std::string(Env::res(e.call("hello")).find("mode")->as_string()), std::string("monitor"));
    const auto sm = e.call("setMode", "{\"mode\":\"balanced\"}", true);
    CHECK(Env::ok(sm));
    CHECK_EQ(Env::res(sm).find("mode")->as_string(), std::string("balanced"));
    CHECK(!Env::res(sm).find("simulation")->as_bool());
    CHECK_EQ(std::string(Env::res(e.call("hello")).find("mode")->as_string()), std::string("balanced"));
    CHECK_EQ(std::string(Env::res(e.call("health")).find("mode")->as_string()), std::string("balanced"));
    CHECK(!Env::res(e.call("status")).find("simulation")->as_bool());
    CHECK(Env::ok(e.call("setMode", "{\"mode\":\"monitor\"}", true)));
    CHECK_EQ(std::string(Env::res(e.call("hello")).find("mode")->as_string()), std::string("monitor"));
    // Mode changes are journaled.
    const auto j = Env::res(e.call("journal", "{\"limit\":50}"));
    int mode_entries = 0;
    for (const auto& t : j.find("entries")->as_array())
        if (json::serialize(t).find("mode") != std::string::npos) ++mode_entries;
    CHECK(mode_entries >= 2);
}

TEST(cmd2_setmode_validation) {
    Env e;
    CHECK_EQ(Env::code(e.call("setMode", "", true)), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("setMode", "{\"mode\":\"turbo\"}", true)), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("setMode", "{\"mode\":1}", true)), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("setMode", "{\"mode\":\"monitor\",\"extra\":1}", true)), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("setMode", "{\"mode\":\"balanced\",\"optIn\":[\"kill\"]}", true)), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("setMode", "{\"mode\":\"balanced\",\"optIn\":\"ecoqos\"}", true)), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("setMode", "{\"mode\":\"balanced\",\"simulation\":\"yes\"}", true)), std::string("bad_args"));
    CHECK(e.rig.eng->mode() == control::Mode::Monitor);
    const auto r = e.call("setMode", "{\"mode\":\"balanced\",\"optIn\":[\"ecoqos\",\"memory_priority\"],\"simulation\":true}", true);
    CHECK(Env::ok(r));
    CHECK(Env::res(r).find("simulation")->as_bool());
    CHECK_EQ(Env::res(r).find("optIn")->size(), std::size_t{2});
    CHECK(e.rig.io.files["/data/config.json"].find("memory_priority") != std::string::npos);
}

TEST(cmd2_sample_history_baselines) {
    Env e;
    CHECK_EQ(Env::code(e.call("sample")), std::string("internal"));  // nothing collected yet
    obs_test::NormalGen gen(5);
    for (int i = 0; i < 200; ++i) e.rig.tick(gen.sample(e.rig.now + kTick));
    const auto s = Env::res(e.call("sample"));
    for (const char* k : {"tsMs", "cpuPercent", "memTotal", "memAvail", "memLoadPercent", "commitTotal", "commitLimit",
                          "hardFaultsPerSec", "diskReadLatencyMs", "diskWriteLatencyMs"})
        CHECK(is_num(s.find(k)));
    CHECK(is_int(s.find("processes")));
    CHECK(s.find("top")->is_array() && s.find("top")->size() == 3);
    const auto& p0 = s.find("top")->as_array()[0];
    CHECK(is_int(p0.find("pid")) && p0.find("name")->is_string() && is_num(p0.find("cpuPercent")));
    CHECK(!p0.find("commandLine") && !p0.find("windowTitle"));

    for (const char* range : {"30m", "24h", "14d"}) {
        const auto h = e.call("history", std::string("{\"metric\":\"cpuPercent\",\"range\":\"") + range + "\"}");
        CHECK(Env::ok(h));
        const auto& hr = Env::res(h);
        CHECK_EQ(hr.find("metric")->as_string(), std::string("cpuPercent"));
        CHECK(hr.find("unit")->is_string());
        const auto& pts = hr.find("points")->as_array();
        CHECK(pts.size() <= 900);
        CHECK(!pts.empty());
        for (std::size_t i = 0; i < pts.size(); ++i) {
            CHECK(pts[i].is_array() && pts[i].size() == 2 && pts[i].as_array()[0].is_number());
            if (i) CHECK(pts[i].as_array()[0].as_double() >= pts[i - 1].as_array()[0].as_double());
        }
    }
    CHECK_EQ(Env::res(e.call("history", "{\"metric\":\"cpuPercent\",\"range\":\"30m\"}")).find("points")->size(),
             std::size_t{200});
    CHECK_EQ(Env::code(e.call("history")), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("history", "{\"metric\":\"nope\",\"range\":\"30m\"}")), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("history", "{\"metric\":\"cpuPercent\",\"range\":\"7d\"}")), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("history", "{\"metric\":\"cpuPercent\",\"range\":\"30m\",\"x\":1}")), std::string("bad_args"));

    const auto b = Env::res(e.call("baselines"));
    const auto* m = b.find("metrics");
    CHECK(m && m->is_object() && m->size() > 0);
    for (const auto& [name, v] : m->as_object()) {
        CHECK(is_num(v.find("median")) && is_num(v.find("mad")) && is_num(v.find("ewma")) && is_int(v.find("samples")) &&
              v.find("ready")->is_bool());
        (void)name;
    }
    // Before any minute has closed, metrics without data are omitted rather than reported as null.
    Env fresh;
    fresh.rig.tick(quiet(kT0 + kTick));
    CHECK_EQ(Env::res(fresh.call("baselines")).find("metrics")->size(), std::size_t{0});
}

TEST(cmd2_lists_limits_and_shapes) {
    Env e;
    obs_test::NormalGen gen(9);
    // 2 h normal + 1 h leak -> an incident; plus some events.
    for (int i = 0; i < 5400; ++i) {
        auto s = gen.sample(e.rig.now + kTick);
        if (i >= 3600) s.top.push_back(proc(5000, "leaky.exe", 1.0, 200.0 + 6.0 * ((i - 3600) / 30)));
        std::vector<telemetry::EventRecord> ev;
        if (i % 600 == 0) {
            telemetry::EventRecord r;
            r.ts_ms = s.ts_ms;
            r.kind = telemetry::EventKind::AppCrash;
            r.provider = "Application Error";
            r.event_id = 1000;
            r.level = 2;
            r.subject = "game.exe";
            r.message = "game.exe crashed";
            ev.push_back(r);
        }
        e.rig.tick(s, ev);
    }
    const auto inc = Env::res(e.call("incidents", "{\"limit\":50}"));
    const auto& list = inc.find("incidents")->as_array();
    CHECK(!list.empty());
    bool leak = false;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& x = list[i];
        CHECK(x.find("id")->is_string() && x.find("symptoms")->is_array() && x.find("evidence")->is_array() &&
              is_num(x.find("confidence")) && x.find("risk")->is_string() && x.find("autoRecoveryAllowed")->is_bool() &&
              x.find("title")->is_string() && x.find("confidenceLevel")->is_string() &&
              x.find("contributors")->is_array() && x.find("recommendedActions")->is_array());
        if (x.find("detector")->as_string() == "memory_growth") leak = true;
        if (i) CHECK(x.find("lastSeen")->as_double() <= list[i - 1].find("lastSeen")->as_double());
    }
    CHECK(leak);
    CHECK(Env::res(e.call("incidents", "{\"limit\":1}")).find("incidents")->size() <= 1);
    CHECK_EQ(Env::code(e.call("incidents", "{\"limit\":\"x\"}")), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("incidents", "{\"limit\":0}")), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("incidents", "{\"limit\":501}")), std::string("bad_args"));

    const auto ev = Env::res(e.call("events", "{\"limit\":100}"));
    const auto& evs = ev.find("events")->as_array();
    CHECK(evs.size() == 9u);
    for (std::size_t i = 0; i < evs.size(); ++i) {
        const auto& x = evs[i];
        CHECK(is_num(x.find("tsMs")) && x.find("kind")->is_string() && x.find("provider")->is_string() &&
              is_int(x.find("eventId")) && is_int(x.find("level")) && x.find("message")->is_string());
        if (i) CHECK(x.find("tsMs")->as_double() <= evs[i - 1].find("tsMs")->as_double());
    }

    const auto rec = Env::res(e.call("recommendations"));
    CHECK(rec.find("items")->is_array());
    const auto pol = Env::res(e.call("policies"));
    CHECK(pol.find("active")->is_array() && pol.find("active")->size() == 0);
    const auto jr = Env::res(e.call("journal"));
    CHECK(jr.find("entries")->is_array());
    CHECK_EQ(Env::code(e.call("journal", "{\"limit\":-1}")), std::string("bad_args"));
    const auto heal = Env::res(e.call("heal"));
    const auto* br = heal.find("breaker");
    CHECK(br && br->find("open")->is_bool() && is_int(br->find("failures")) && is_num(br->find("openedAt")));
    CHECK(heal.find("pending")->is_array() && heal.find("history")->is_array());
}

TEST(cmd2_recommendations_and_policies_in_balanced) {
    Env e;
    CHECK(Env::ok(e.call("setMode", "{\"mode\":\"balanced\",\"optIn\":[\"ecoqos\"]}", true)));
    for (int i = 0; i < 60 && e.rig.act.applies == 0; ++i) e.rig.tick(contended(e.rig.now + kTick));
    CHECK_EQ(e.rig.act.applies, 1);
    const auto pol = Env::res(e.call("policies"));
    CHECK_EQ(pol.find("active")->size(), std::size_t{1});
    std::string tx;
    if (pol.find("active")->size() == 1) {
        const auto& p = pol.find("active")->as_array()[0];
        CHECK(p.find("txId")->is_string() && p.find("kind")->as_string() == "ecoqos" &&
              p.find("target")->find("name")->as_string() == "SearchProtocolHost.exe" && is_num(p.find("original")) &&
              is_num(p.find("appliedAt")) && is_num(p.find("verifyUntil")) && p.find("state")->is_string());
        tx = p.find("txId")->as_string();
    }
    const auto rec = Env::res(e.call("recommendations"));
    CHECK(rec.find("items")->size() >= 1);
    for (const auto& x : rec.find("items")->as_array()) {
        CHECK(x.find("id")->is_string());
        CHECK(x.find("area")->as_string() == "cpu" || x.find("area")->as_string() == "memory");
        CHECK(x.find("target")->is_object() && x.find("action")->is_string() && x.find("reason")->is_string() &&
              is_num(x.find("risk")) && x.find("wouldApply")->is_bool() && x.find("workload")->is_string() &&
              x.find("evidence")->is_array() && x.find("expectedEffect")->is_string() && is_num(x.find("cost")));
    }
    // disablePolicy: unknown -> restored:false; known -> restored:true.
    CHECK(!Env::res(e.call("disablePolicy", "{\"txId\":999999999}", true)).find("restored")->as_bool());
    CHECK(!Env::res(e.call("disablePolicy", "{\"txId\":\"tx-nope\"}", true)).find("restored")->as_bool());
    CHECK_EQ(Env::code(e.call("disablePolicy", "{}", true)), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("disablePolicy", "{\"txId\":\"bad id with spaces\"}", true)), std::string("bad_args"));
    CHECK(Env::res(e.call("disablePolicy", "{\"txId\":\"" + tx + "\"}", true)).find("restored")->as_bool());
    CHECK_EQ(e.rig.act.restores, 1);
    const auto rb = Env::res(e.call("rollbackAll", "", true));
    CHECK_EQ(rb.find("restored")->as_int().value_or(-1), 0);
    CHECK_EQ(rb.find("failed")->as_int().value_or(-1), 0);
}

TEST(cmd2_report_foreground_uses_token_session) {
    Env e;
    CHECK(Env::ok(e.call("reportForeground", "{\"pid\":100}", false, 1)));
    CHECK(!e.fg_calls.empty() && e.fg_calls.back() == std::make_pair(100u, 1u));
    CHECK_EQ(Env::code(e.call("reportForeground", "{\"pid\":100}", false, 2)), std::string("bad_args"));
    CHECK(Env::ok(e.call("reportForeground", "{\"pid\":300}", false, 0)));  // ignored but accepted
    CHECK_EQ(Env::code(e.call("reportForeground", "{\"pid\":2147483632}")), std::string("bad_args"));
    const auto before = e.fg_calls.size();
    CHECK_EQ(Env::code(e.call("reportForeground", "{\"pid\":0}")), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("reportForeground", "{\"pid\":4}")), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("reportForeground", "{\"pid\":\"100\"}")), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("reportForeground", "{}")), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("reportForeground", "{\"pid\":100,\"session\":1}")), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("reportForeground", "{\"pid\":4294967296}")), std::string("bad_args"));
    CHECK_EQ(e.fg_calls.size(), before);  // invalid args never reach the platform
    CHECK_EQ(Env::code(e.call("reportForeground", "{\"pid\":100}", false, protocol::kUnknownSession)),
             std::string("forbidden"));
}

TEST(cmd2_topology_keeps_phase1_fields_and_adds_cpusets) {
    Env e;
    const auto t = Env::res(e.call("topology"));
    for (const char* k : {"logicalProcessors", "cores", "packages", "numaNodes", "groups", "l3Caches"}) CHECK(is_int(t.find(k)));
    CHECK(t.find("hybrid")->is_bool() && t.find("efficiencyClasses")->is_array());
    const auto* sets = t.find("cpuSets");
    CHECK(sets && sets->size() == 8);
    for (const auto& c : sets->as_array())
        for (const char* k : {"id", "group", "core", "efficiencyClass", "numa"}) CHECK(is_int(c.find(k)));
}

TEST(cmd2_approve_and_export_diagnostics) {
    Env e;
    CHECK_EQ(Env::code(e.call("approve", "{\"id\":\"no-such-approval\"}", true)), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("approve", "{}", true)), std::string("bad_args"));
    std::vector<telemetry::EventRecord> ev;
    for (int i = 0; i < 3; ++i) {
        telemetry::EventRecord r;
        r.ts_ms = kT0 + kTick + i;
        r.kind = telemetry::EventKind::ServiceFailure;
        r.provider = "Service Control Manager";
        r.event_id = 7031;
        r.level = 2;
        r.subject = "vendorsvc";
        r.message = "vendorsvc terminated";
        ev.push_back(r);
    }
    e.rig.tick(quiet(kT0 + kTick), ev);
    const auto heal = Env::res(e.call("heal"));
    CHECK_EQ(heal.find("pending")->size(), std::size_t{1});
    std::string id;
    if (heal.find("pending")->size() == 1) {
        const auto& a = heal.find("pending")->as_array()[0];
        for (const char* k : {"id", "action", "target", "trigger", "risk"}) CHECK(a.find(k) && a.find(k)->is_string());
        CHECK(a.find("evidence")->is_array() && is_num(a.find("createdMs")) && is_num(a.find("expiresMs")));
        CHECK_EQ(a.find("action")->as_string(), std::string("restart_service"));
        id = a.find("id")->as_string();
    }
    CHECK_EQ(Env::code(e.call("approve", "{\"id\":\"" + id + "\"}")), std::string("forbidden"));
    const auto ap = e.call("approve", "{\"id\":\"" + id + "\"}", true);
    CHECK(Env::ok(ap));
    CHECK(Env::res(ap).find("txId")->is_string() && !Env::res(ap).find("txId")->as_string().empty());
    CHECK(!e.rig.exec.calls.empty() && e.rig.exec.calls.back().first == heal::Action::RestartService);
    CHECK_EQ(Env::code(e.call("approve", "{\"id\":\"" + id + "\"}", true)), std::string("bad_args"));  // used up

    const auto ex = e.call("exportDiagnostics", "", true);
    CHECK(Env::ok(ex));
    CHECK_EQ(Env::res(ex).find("path")->as_string(), std::string("/data/diagnostics.json"));
    auto parsed = json::parse(e.rig.io.files["/data/diagnostics.json"]);
    CHECK(parsed.ok());
    if (parsed.ok()) {
        const auto& d = *parsed.value;
        for (const char* k : {"status", "sample", "baselines", "incidents", "events", "journal", "heal", "topology"})
            CHECK(d.find(k) != nullptr);
        const std::string text = e.rig.io.files["/data/diagnostics.json"];
        CHECK(text.find("commandLine") == std::string::npos && text.find("windowTitle") == std::string::npos);
    }
}

TEST(cmd2_responses_stay_below_the_frame_limit) {
    Env e;
    std::vector<telemetry::EventRecord> ev;
    for (int i = 0; i < 2000; ++i) {
        telemetry::EventRecord r;
        r.ts_ms = kT0 + i;
        r.kind = telemetry::EventKind::Other;
        r.provider = std::string(100, 'p');
        r.subject = std::string(96, 's');
        r.message = std::string(200, 'm');
        ev.push_back(r);
    }
    e.rig.tick(quiet(kT0 + 10'000), ev);
    std::size_t bytes = 0;
    const auto r = e.call("events", "{\"limit\":2000}", false, 1, &bytes);
    CHECK(Env::ok(r));
    CHECK(bytes <= framing::kMaxResponsePayload);
    CHECK(Env::res(r).find("events")->size() > 100);
}

// ---- kernel tracing commands (docs/core/etw.md) -------------------------------------------------
TEST(cmd2_etw_commands_without_platform_session) {
    Env e;
    for (const char* c : {"latency", "diskActivity", "hardFaults", "setEtw"}) CHECK(e.d.has(c));
    // status gains etw:{ running, flags[], eventsLost, costPercent }.
    const auto st = Env::res(e.call("status"));
    const auto* etw = st.find("etw");
    CHECK(etw && etw->find("running") && !etw->find("running")->as_bool());
    CHECK(etw && etw->find("flags") && etw->find("flags")->is_array());
    CHECK(etw && is_int(etw->find("eventsLost")) && is_num(etw->find("costPercent")));
    const auto lat = Env::res(e.call("latency"));
    CHECK(lat.find("status") && lat.find("system") && lat.find("drivers") && lat.find("drivers")->size() == 0);
    CHECK(Env::res(e.call("diskActivity")).find("processes")->is_array());
    CHECK(Env::res(e.call("hardFaults")).find("processes")->is_array());
    CHECK_EQ(Env::code(e.call("latency", "{\"x\":1}")), std::string("bad_args"));
    // setEtw: admin only (forbidden before argument checks), strict boolean.
    CHECK_EQ(Env::code(e.call("setEtw", "{\"bogus\":1}")), std::string("forbidden"));
    CHECK_EQ(Env::code(e.call("setEtw", "{}", true)), std::string("bad_args"));
    CHECK_EQ(Env::code(e.call("setEtw", "{\"enabled\":1}", true)), std::string("bad_args"));
    const auto r = e.call("setEtw", "{\"enabled\":false}", true);
    CHECK(Env::ok(r));
    CHECK(!Env::res(r).find("enabled")->as_bool());
    CHECK(!Env::res(r).find("running")->as_bool());
    CHECK(e.rig.io.files["/data/config.json"].find("\"etw\":false") != std::string::npos);
    CHECK(!e.rig.eng->etw_enabled());
}

TEST(cmd2_etw_commands_with_platform_session) {
    Env e;
    bool enabled = true;
    e.platform.etw_status = [&] {
        kstats::EtwStatus s;
        s.enabled = enabled;
        s.running = enabled;
        s.flags = kstats::flags_for_level(enabled ? 0 : 3);
        s.events_lost = 2;
        s.cost_percent = 0.3;
        return s;
    };
    e.platform.etw_latency = [] {
        kstats::LatencyView v;
        kstats::DriverStats d;
        d.name = "ndis.sys";
        d.dpc_count = 5;
        d.dpc_us = 900;
        d.max_us = 400;
        d.histogram[3] = 5;
        v.drivers.push_back(d);
        v.dpc_percent = 0.1;
        return v;
    };
    e.platform.etw_disk = [] {
        return std::vector<kstats::DiskProcessView>{{1234, "backup.exe", 1000.0, 2000.0, 7, 1.5, 9.0}};
    };
    e.platform.etw_faults = [] { return std::vector<kstats::FaultProcessView>{{1234, "backup.exe", 55.5}}; };
    e.platform.set_etw = [&](bool on) {
        enabled = on;
        return e.platform.etw_status();
    };
    const json::Value status = Env::res(e.call("status"));  // copy: never point into a temporary
    const json::Value* st = status.find("etw");
    CHECK(st && st->find("running")->as_bool() && st->find("flags")->size() == 7);
    const auto lat = Env::res(e.call("latency"));
    CHECK(lat.find("drivers")->size() == 1);
    const auto& d = lat.find("drivers")->as_array()[0];
    CHECK_EQ(d.find("name")->as_string(), std::string("ndis.sys"));
    CHECK(d.find("histogram")->size() == 8);
    CHECK(lat.find("status")->find("eventsLost")->as_int() == 2);
    const auto disk = Env::res(e.call("diskActivity")).find("processes")->as_array();
    CHECK(disk.size() == 1 && disk[0].find("pid")->as_int() == 1234 && disk[0].find("writeBps")->as_double() == 2000.0);
    const auto hf = Env::res(e.call("hardFaults")).find("processes")->as_array();
    CHECK(hf.size() == 1 && hf[0].find("faultsPerSec")->as_double() == 55.5);
    const auto off = Env::res(e.call("setEtw", "{\"enabled\":false}", true));
    CHECK(!off.find("enabled")->as_bool() && !off.find("running")->as_bool());
    CHECK(!Env::res(e.call("status")).find("etw")->find("running")->as_bool());
    const auto on = Env::res(e.call("setEtw", "{\"enabled\":true}", true));
    CHECK(on.find("enabled")->as_bool() && on.find("running")->as_bool());
}
