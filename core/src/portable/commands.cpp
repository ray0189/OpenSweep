#include "commands.hpp"

#include <cmath>
#include <cstdio>

namespace opensweep::commands {

bool TopologyInfo::hybrid() const noexcept {
    std::size_t distinct = 0;
    for (auto n : efficiency_classes) {
        if (n > 0) ++distinct;
    }
    return distinct > 1;
}

namespace {

// Howard Hinnant's days_from_civil inverse: days since 1970-01-01 -> y/m/d (proleptic Gregorian).
void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

std::int64_t uptime_sec(const ServiceInfo& info) {
    const auto up = std::chrono::steady_clock::now() - info.started_steady;
    const auto s = std::chrono::duration_cast<std::chrono::seconds>(up).count();
    return s < 0 ? 0 : static_cast<std::int64_t>(s);
}

}  // namespace

std::string format_iso8601_utc(std::int64_t unix_ms) {
    const std::int64_t days = floor_div(unix_ms, 86400000);
    const std::int64_t ms_of_day = unix_ms - days * 86400000;
    std::int64_t y = 0;
    unsigned mo = 0, d = 0;
    civil_from_days(days, y, mo, d);
    const auto hh = static_cast<int>(ms_of_day / 3600000);
    const auto mi = static_cast<int>((ms_of_day / 60000) % 60);
    const auto ss = static_cast<int>((ms_of_day / 1000) % 60);
    const auto ms = static_cast<int>(ms_of_day % 1000);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ", static_cast<long long>(y), mo, d, hh,
                  mi, ss, ms);
    return buf;
}

std::int64_t now_unix_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

json::Value build_hello(const ServiceInfo& info, std::uint32_t pid) {
    json::Value r = json::Value::object();
    r.set("service", kServiceName);
    r.set("version", kVersion);
    r.set("protocol", protocol::kProtocolVersion);
    r.set("pid", pid);
    r.set("mode", kMode);
    r.set("startedAt", format_iso8601_utc(info.started_at_ms));
    r.set("uptimeSec", uptime_sec(info));
    return r;
}

json::Value build_health(const ServiceInfo& info, const SelfProcessInfo& self, const ServerCounters& counters) {
    json::Value r = json::Value::object();
    r.set("status", "running");
    r.set("mode", kMode);
    r.set("uptimeSec", uptime_sec(info));
    r.set("workingSetBytes", self.working_set);
    r.set("privateBytes", self.private_bytes);
    r.set("cpuTimeMs", self.cpu_time_ms);
    r.set("clients", counters.clients.load(std::memory_order_relaxed));
    r.set("requests", counters.requests.load(std::memory_order_relaxed));
    r.set("errors", counters.errors.load(std::memory_order_relaxed));
    return r;
}

json::Value build_metrics(const MetricsSnapshot& m) {
    double pct = m.cpu_percent;
    if (!std::isfinite(pct) || pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    pct = std::round(pct * 10.0) / 10.0;  // one decimal is plenty for a 1 s sample

    json::Value cpu = json::Value::object();
    cpu.set("percent", pct);

    json::Value mem = json::Value::object();
    mem.set("totalPhys", m.memory.total_phys);
    mem.set("availPhys", m.memory.avail_phys);
    mem.set("loadPercent", m.memory.load_percent);
    mem.set("commitTotal", m.memory.commit_total);
    mem.set("commitLimit", m.memory.commit_limit);
    mem.set("commitPeak", m.memory.commit_peak);
    mem.set("systemCache", m.memory.system_cache);
    mem.set("kernelPaged", m.memory.kernel_paged);
    mem.set("kernelNonpaged", m.memory.kernel_nonpaged);

    json::Value sys = json::Value::object();
    sys.set("processes", m.processes);
    sys.set("threads", m.threads);
    sys.set("handles", m.handles);

    json::Value r = json::Value::object();
    r.set("timestamp", format_iso8601_utc(m.timestamp_ms));
    r.set("cpu", std::move(cpu));
    r.set("memory", std::move(mem));
    r.set("system", std::move(sys));
    r.set("pageSize", m.page_size);
    return r;
}

json::Value build_topology(const TopologyInfo& t) {
    json::Value classes = json::Value::array();
    for (auto n : t.efficiency_classes) classes.push_back(n);
    json::Value r = json::Value::object();
    r.set("logicalProcessors", t.logical_processors);
    r.set("cores", t.cores);
    r.set("packages", t.packages);
    r.set("numaNodes", t.numa_nodes);
    r.set("groups", t.groups);
    r.set("hybrid", t.hybrid());
    r.set("efficiencyClasses", std::move(classes));
    r.set("l3Caches", t.l3_caches);
    return r;
}

void register_phase1(protocol::Dispatcher& dispatcher, SystemProvider& provider, const ServiceInfo& info,
                     const ServerCounters& counters) {
    using protocol::Capability;
    using protocol::ClientContext;
    using protocol::CommandError;
    using protocol::ErrorCode;
    using protocol::Request;

    dispatcher.add("hello", Capability::Read, [&provider, &info](const Request&, const ClientContext&) {
        return build_hello(info, provider.pid());
    });
    dispatcher.add("health", Capability::Read, [&provider, &info, &counters](const Request&, const ClientContext&) {
        SelfProcessInfo self;
        if (!provider.self_process(self)) throw CommandError(ErrorCode::Internal, "process information unavailable");
        return build_health(info, self, counters);
    });
    dispatcher.add("metrics", Capability::Read, [&provider](const Request&, const ClientContext&) {
        MetricsSnapshot m;
        if (!provider.metrics(m)) throw CommandError(ErrorCode::Internal, "metrics unavailable");
        return build_metrics(m);
    });
    dispatcher.add("topology", Capability::Read, [&provider](const Request&, const ClientContext&) {
        TopologyInfo t;
        if (!provider.topology(t)) throw CommandError(ErrorCode::Internal, "topology unavailable");
        return build_topology(t);
    });
}

}  // namespace opensweep::commands
