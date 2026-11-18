#include <string>

#include "commands.hpp"
#include "framing.hpp"
#include "json.hpp"
#include "protocol.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using namespace opensweep::commands;

namespace {

class FakeProvider : public SystemProvider {
public:
    bool fail = false;
    bool metrics(MetricsSnapshot& m) override {
        if (fail) return false;
        m.timestamp_ms = 1700000000123;
        m.cpu_percent = 12.345;
        m.memory.total_phys = 17179869184ull;  // 16 GiB
        m.memory.avail_phys = 8589934592ull;
        m.memory.load_percent = 50;
        m.memory.commit_total = 1;
        m.memory.commit_limit = 2;
        m.memory.commit_peak = 3;
        m.memory.system_cache = 4;
        m.memory.kernel_paged = 5;
        m.memory.kernel_nonpaged = 6;
        m.processes = 200;
        m.threads = 3000;
        m.handles = 90000;
        m.page_size = 4096;
        return true;
    }
    bool topology(TopologyInfo& t) override {
        if (fail) return false;
        t.logical_processors = 20;
        t.cores = 14;
        t.packages = 1;
        t.numa_nodes = 1;
        t.groups = 1;
        t.efficiency_classes = {8, 6};
        t.l3_caches = 1;
        return true;
    }
    bool self_process(SelfProcessInfo& s) override {
        if (fail) return false;
        s.working_set = 4000000;
        s.private_bytes = 2000000;
        s.cpu_time_ms = 15;
        return true;
    }
    std::uint32_t pid() override { return 4242; }
};

json::Value call(const protocol::Dispatcher& d, const std::string& cmd) {
    const auto out = d.handle(R"({"v":1,"id":1,"cmd":")" + cmd + "\"}", {}, framing::kMaxResponsePayload);
    return *json::parse(out.response).value;
}

}  // namespace

TEST(commands_iso8601) {
    CHECK_EQ(format_iso8601_utc(0), std::string("1970-01-01T00:00:00.000Z"));
    CHECK_EQ(format_iso8601_utc(1700000000123), std::string("2023-11-14T22:13:20.123Z"));
    CHECK_EQ(format_iso8601_utc(951782400000), std::string("2000-02-29T00:00:00.000Z"));  // leap day
    CHECK_EQ(format_iso8601_utc(-1), std::string("1969-12-31T23:59:59.999Z"));
    CHECK(now_unix_ms() > 1700000000000);
}

TEST(commands_hybrid_detection) {
    TopologyInfo t;
    CHECK(!t.hybrid());
    t.efficiency_classes = {8};
    CHECK(!t.hybrid());
    t.efficiency_classes = {0, 8};
    CHECK(!t.hybrid());
    t.efficiency_classes = {4, 8};
    CHECK(t.hybrid());
}

TEST(commands_phase1_results) {
    FakeProvider p;
    ServiceInfo info;
    info.started_at_ms = 1700000000000;
    ServerCounters counters;
    counters.requests = 10;
    counters.errors = 2;
    counters.clients = 1;
    protocol::Dispatcher d;
    register_phase1(d, p, info, counters);

    auto hello = call(d, "hello");
    CHECK(hello.find("ok")->as_bool());
    const json::Value* h = hello.find("result");
    CHECK_EQ(h->find("service")->as_string(), std::string("OpenSweepCore"));
    CHECK_EQ(h->find("version")->as_string(), std::string("0.1.0"));
    CHECK(h->find("protocol")->as_int() == 1);
    CHECK(h->find("pid")->as_int() == 4242);
    CHECK_EQ(h->find("mode")->as_string(), std::string("monitor"));
    CHECK_EQ(h->find("startedAt")->as_string(), std::string("2023-11-14T22:13:20.000Z"));
    CHECK(h->find("uptimeSec")->is_int());

    auto health = call(d, "health");
    const json::Value* hl = health.find("result");
    CHECK_EQ(hl->find("status")->as_string(), std::string("running"));
    CHECK_EQ(hl->find("mode")->as_string(), std::string("monitor"));
    CHECK(hl->find("workingSetBytes")->as_int() == 4000000);
    CHECK(hl->find("privateBytes")->as_int() == 2000000);
    CHECK(hl->find("cpuTimeMs")->as_int() == 15);
    CHECK(hl->find("clients")->as_int() == 1);
    CHECK(hl->find("requests")->as_int() == 10);
    CHECK(hl->find("errors")->as_int() == 2);
    CHECK(hl->find("uptimeSec") != nullptr);

    auto metrics = call(d, "metrics");
    const json::Value* m = metrics.find("result");
    CHECK_EQ(m->find("timestamp")->as_string(), std::string("2023-11-14T22:13:20.123Z"));
    CHECK_EQ(m->find("cpu")->find("percent")->as_double(), 12.3);
    const json::Value* mem = m->find("memory");
    CHECK(mem->find("totalPhys")->as_int() == 17179869184);
    CHECK(mem->find("availPhys")->as_int() == 8589934592);
    CHECK(mem->find("loadPercent")->as_int() == 50);
    for (const char* k : {"commitTotal", "commitLimit", "commitPeak", "systemCache", "kernelPaged", "kernelNonpaged"})
        CHECK(mem->find(k) != nullptr && mem->find(k)->is_int());
    CHECK(m->find("system")->find("processes")->as_int() == 200);
    CHECK(m->find("system")->find("threads")->as_int() == 3000);
    CHECK(m->find("system")->find("handles")->as_int() == 90000);
    CHECK(m->find("pageSize")->as_int() == 4096);

    auto topo = call(d, "topology");
    const json::Value* t = topo.find("result");
    CHECK(t->find("logicalProcessors")->as_int() == 20);
    CHECK(t->find("cores")->as_int() == 14);
    CHECK(t->find("packages")->as_int() == 1);
    CHECK(t->find("numaNodes")->as_int() == 1);
    CHECK(t->find("groups")->as_int() == 1);
    CHECK(t->find("hybrid")->as_bool() == true);
    CHECK_EQ(json::serialize(*t->find("efficiencyClasses")), std::string("[8,6]"));
    CHECK(t->find("l3Caches")->as_int() == 1);
}

TEST(commands_cpu_percent_clamped) {
    MetricsSnapshot m;
    m.cpu_percent = 150.0;
    CHECK_EQ(build_metrics(m).find("cpu")->find("percent")->as_double(), 100.0);
    m.cpu_percent = -3.0;
    CHECK_EQ(build_metrics(m).find("cpu")->find("percent")->as_double(), 0.0);
}

TEST(commands_provider_failure_is_internal) {
    FakeProvider p;
    p.fail = true;
    ServiceInfo info;
    ServerCounters counters;
    protocol::Dispatcher d;
    register_phase1(d, p, info, counters);
    for (const char* c : {"health", "metrics", "topology"}) {
        auto r = call(d, c);
        CHECK(!r.find("ok")->as_bool());
        CHECK_EQ(r.find("error")->find("code")->as_string(), std::string("internal"));
    }
    CHECK(call(d, "hello").find("ok")->as_bool());
}
