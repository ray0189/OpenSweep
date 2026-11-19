#include "trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

namespace opensweep::trace {

namespace {

constexpr MetricInfo kMetrics[kMetricCount] = {
    {Metric::CpuPercent, "cpuPercent", "%"},
    {Metric::CpuQueueLength, "cpuQueueLength", "threads"},
    {Metric::ContextSwitchesPerSec, "contextSwitchesPerSec", "/s"},
    {Metric::DpcPercent, "dpcPercent", "%"},
    {Metric::InterruptPercent, "interruptPercent", "%"},
    {Metric::MemTotal, "memTotal", "bytes"},
    {Metric::MemAvail, "memAvail", "bytes"},
    {Metric::MemLoadPercent, "memLoadPercent", "%"},
    {Metric::CommitTotal, "commitTotal", "bytes"},
    {Metric::CommitLimit, "commitLimit", "bytes"},
    {Metric::CommitPercent, "commitPercent", "%"},
    {Metric::SystemCache, "systemCache", "bytes"},
    {Metric::KernelPaged, "kernelPaged", "bytes"},
    {Metric::KernelNonpaged, "kernelNonpaged", "bytes"},
    {Metric::CompressedBytes, "compressedBytes", "bytes"},
    {Metric::HardFaultsPerSec, "hardFaultsPerSec", "/s"},
    {Metric::PageFaultsPerSec, "pageFaultsPerSec", "/s"},
    {Metric::DiskReadLatencyMs, "diskReadLatencyMs", "ms"},
    {Metric::DiskWriteLatencyMs, "diskWriteLatencyMs", "ms"},
    {Metric::DiskQueueLength, "diskQueueLength", "count"},
    {Metric::DiskBytesPerSec, "diskBytesPerSec", "B/s"},
    {Metric::CpuFreqPercent, "cpuFreqPercent", "%"},
    {Metric::Processes, "processes", "count"},
    {Metric::Threads, "threads", "count"},
    {Metric::Handles, "handles", "count"},
};

constexpr float kNaNf = std::numeric_limits<float>::quiet_NaN();

Values values_of(const Sample& s) noexcept {
    Values v{};
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        v[i] = static_cast<float>(metric_value(s, static_cast<Metric>(i)));
    }
    return v;
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// ---- little-endian byte helpers -------------------------------------------------------------
void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}
void put_i64(std::string& out, std::int64_t v) {
    const auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((u >> (8 * i)) & 0xFFu));
}
void put_f32(std::string& out, float f) {
    std::uint32_t u = 0;
    static_assert(sizeof(float) == 4);
    std::memcpy(&u, &f, 4);
    put_u32(out, u);
}
std::uint32_t get_u32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::int64_t get_i64(const unsigned char* p) noexcept {
    std::uint64_t u = 0;
    for (int i = 7; i >= 0; --i) u = (u << 8) | p[i];
    return static_cast<std::int64_t>(u);
}
float get_f32(const unsigned char* p) noexcept {
    const std::uint32_t u = get_u32(p);
    float f = 0.0f;
    std::memcpy(&f, &u, 4);
    return f;
}

constexpr char kMagic[4] = {'O', 'S', 'T', 'R'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderSize = 20;       // magic, version, metric_count, block_count, crc
constexpr std::size_t kBlockHeaderSize = 16;  // type, rows, payload_len, crc(type|rows|len|payload)
constexpr std::uint32_t kBlockNames = 1;
constexpr std::uint32_t kBlockTier0 = 10;
constexpr std::uint32_t kBlockTier1 = 11;
constexpr std::uint32_t kBlockTier2 = 12;
constexpr std::size_t kMaxTraceFile = 8u * 1024u * 1024u;
constexpr std::size_t kMaxEventsFile = 8u * 1024u * 1024u;
constexpr std::size_t kMaxFileMetrics = 256;

void put_block(std::string& out, std::uint32_t type, std::uint32_t rows, const std::string& payload) {
    std::string hdr;
    put_u32(hdr, type);
    put_u32(hdr, rows);
    put_u32(hdr, static_cast<std::uint32_t>(payload.size()));
    std::uint32_t c = crc32(hdr);
    c = crc32(payload, c);
    out += hdr;
    put_u32(out, c);
    out += payload;
}

std::filesystem::path to_path(const std::string& utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string_view trim_line(std::string_view line) noexcept {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    return line;
}

template <typename F>
void for_each_line(std::string_view text, F&& f) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        f(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
std::span<const MetricInfo> metrics() noexcept { return {kMetrics, kMetricCount}; }

const MetricInfo& info(Metric m) noexcept {
    const auto i = index(m);
    return kMetrics[i < kMetricCount ? i : 0];
}

std::optional<Metric> metric_from_name(std::string_view name) noexcept {
    for (const auto& mi : kMetrics) {
        if (name == mi.name) return mi.metric;
    }
    return std::nullopt;
}

double metric_value(const Sample& s, Metric m) noexcept {
    switch (m) {
        case Metric::CpuPercent: return s.cpu_percent;
        case Metric::CpuQueueLength: return s.cpu_queue_length;
        case Metric::ContextSwitchesPerSec: return s.context_switches_per_sec;
        case Metric::DpcPercent: return s.dpc_percent;
        case Metric::InterruptPercent: return s.interrupt_percent;
        case Metric::MemTotal: return static_cast<double>(s.mem_total);
        case Metric::MemAvail: return static_cast<double>(s.mem_avail);
        case Metric::MemLoadPercent: return static_cast<double>(s.mem_load_percent);
        case Metric::CommitTotal: return static_cast<double>(s.commit_total);
        case Metric::CommitLimit: return static_cast<double>(s.commit_limit);
        case Metric::CommitPercent:
            if (s.commit_limit == 0) return std::numeric_limits<double>::quiet_NaN();
            return static_cast<double>(s.commit_total) / static_cast<double>(s.commit_limit) * 100.0;
        case Metric::SystemCache: return static_cast<double>(s.system_cache);
        case Metric::KernelPaged: return static_cast<double>(s.kernel_paged);
        case Metric::KernelNonpaged: return static_cast<double>(s.kernel_nonpaged);
        case Metric::CompressedBytes: return static_cast<double>(s.compressed_bytes);
        case Metric::HardFaultsPerSec: return s.hard_faults_per_sec;
        case Metric::PageFaultsPerSec: return s.page_faults_per_sec;
        case Metric::DiskReadLatencyMs: return s.disk_read_latency_ms;
        case Metric::DiskWriteLatencyMs: return s.disk_write_latency_ms;
        case Metric::DiskQueueLength: return s.disk_queue_length;
        case Metric::DiskBytesPerSec: return s.disk_bytes_per_sec;
        case Metric::CpuFreqPercent: return s.cpu_freq_percent;
        case Metric::Processes: return static_cast<double>(s.processes);
        case Metric::Threads: return static_cast<double>(s.threads);
        case Metric::Handles: return static_cast<double>(s.handles);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

std::optional<Range> range_from_string(std::string_view s) noexcept {
    if (s == "30m") return Range::Minutes30;
    if (s == "24h") return Range::Hours24;
    if (s == "14d") return Range::Days14;
    return std::nullopt;
}

const char* to_string(Range r) noexcept {
    switch (r) {
        case Range::Minutes30: return "30m";
        case Range::Hours24: return "24h";
        case Range::Days14: return "14d";
    }
    return "30m";
}

// ---------------------------------------------------------------------------------------------
std::uint32_t crc32(std::string_view data, std::uint32_t seed) noexcept {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = seed ^ 0xFFFFFFFFu;
    for (char ch : data) c = table[(c ^ static_cast<unsigned char>(ch)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string jsonl_seal(const json::Value& obj) {
    std::string s = json::serialize(obj.is_object() ? obj : json::Value::object());
    const std::uint32_t c = crc32(s);
    s.pop_back();  // '}'
    if (s.size() > 1) s.push_back(',');
    static constexpr char hex[] = "0123456789abcdef";
    s += "\"crc\":\"";
    for (int i = 7; i >= 0; --i) s.push_back(hex[(c >> (4 * i)) & 0xFu]);
    s += "\"}\n";
    return s;
}

std::optional<json::Value> jsonl_open(std::string_view line) {
    line = trim_line(line);
    constexpr std::size_t kTail = 17;  // "crc":"hhhhhhhh"}
    if (line.size() < kTail + 1 || line.front() != '{') return std::nullopt;
    const std::string_view tail = line.substr(line.size() - kTail);
    if (tail.substr(0, 7) != "\"crc\":\"" || tail.substr(15) != "\"}") return std::nullopt;
    std::uint32_t want = 0;
    for (std::size_t i = 7; i < 15; ++i) {
        const char ch = tail[i];
        std::uint32_t d = 0;
        if (ch >= '0' && ch <= '9') d = static_cast<std::uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<std::uint32_t>(ch - 'a' + 10);
        else return std::nullopt;
        want = (want << 4) | d;
    }
    const char sep = line[line.size() - kTail - 1];
    std::string payload;
    if (sep == ',') {
        payload.assign(line.substr(0, line.size() - kTail - 1));
        payload.push_back('}');
    } else if (sep == '{' && line.size() == kTail + 1) {
        payload = "{}";
    } else {
        return std::nullopt;
    }
    if (crc32(payload) != want) return std::nullopt;
    auto r = json::parse(payload);
    if (!r.ok() || !r.value->is_object()) return std::nullopt;
    return std::move(*r.value);
}

// ---------------------------------------------------------------------------------------------
bool StdFileIO::read_all(const std::string& path, std::string& out, std::size_t max_bytes) {
    std::error_code ec;
    const auto p = to_path(path);
    const auto size = std::filesystem::file_size(p, ec);
    if (ec || size > max_bytes) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(static_cast<std::size_t>(size), '\0');
    if (size > 0) f.read(out.data(), static_cast<std::streamsize>(size));
    if (!f && !f.eof()) return false;
    out.resize(static_cast<std::size_t>(f.gcount() >= 0 ? f.gcount() : 0));
    return true;
}

bool StdFileIO::write_atomic(const std::string& path, std::string_view data) {
    const auto p = to_path(path);
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) {
            f.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool StdFileIO::append(const std::string& path, std::string_view data) {
    std::ofstream f(to_path(path), std::ios::binary | std::ios::app);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    return static_cast<bool>(f);
}

// ---------------------------------------------------------------------------------------------
std::optional<EventKind> event_kind_from_string(std::string_view s) noexcept {
    for (int i = 0; i <= static_cast<int>(EventKind::Other); ++i) {
        const auto k = static_cast<EventKind>(i);
        if (s == telemetry::to_string(k)) return k;
    }
    return std::nullopt;
}

json::Value event_to_json(const EventRecord& e) {
    json::Value v = json::Value::object();
    v.set("ts", e.ts_ms);
    v.set("kind", telemetry::to_string(e.kind));
    v.set("provider", e.provider);
    v.set("eventId", e.event_id);
    v.set("level", static_cast<std::uint32_t>(e.level));
    v.set("subject", e.subject);
    v.set("message", e.message);
    v.set("value", std::isfinite(e.value) ? e.value : 0.0);
    return v;
}

std::optional<EventRecord> event_from_json(const json::Value& v) {
    if (!v.is_object()) return std::nullopt;
    constexpr std::size_t kMaxStr = 1024;
    EventRecord e;
    const auto* ts = v.find("ts");
    const auto* kind = v.find("kind");
    if (!ts || !ts->as_int() || !kind || !kind->is_string()) return std::nullopt;
    e.ts_ms = *ts->as_int();
    e.kind = event_kind_from_string(kind->as_string()).value_or(EventKind::Other);
    auto str = [&](const char* key, std::string& out) {
        const auto* f = v.find(key);
        if (!f) return true;
        if (!f->is_string() || f->as_string().size() > kMaxStr) return false;
        out = f->as_string();
        return true;
    };
    if (!str("provider", e.provider) || !str("subject", e.subject) || !str("message", e.message))
        return std::nullopt;
    if (const auto* id = v.find("eventId")) {
        const auto i = id->as_int();
        if (!i || *i < 0 || *i > 0xFFFFFFFFLL) return std::nullopt;
        e.event_id = static_cast<std::uint32_t>(*i);
    }
    if (const auto* lv = v.find("level")) {
        const auto i = lv->as_int();
        if (!i || *i < 0 || *i > 255) return std::nullopt;
        e.level = static_cast<std::uint8_t>(*i);
    }
    if (const auto* val = v.find("value")) {
        if (!val->is_number() || !std::isfinite(val->as_double())) return std::nullopt;
        e.value = val->as_double();
    }
    return e;
}

// ---------------------------------------------------------------------------------------------
void Trace::Acc::reset() noexcept {
    bucket = -1;
    samples = 0;
    sum.fill(0.0);
    n.fill(0);
    max.fill(kNaNf);
}

void Trace::Acc::add(const Values& mean, const Values& max_in, std::uint32_t weight) noexcept {
    samples += weight;
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        if (std::isfinite(mean[i])) {
            sum[i] += static_cast<double>(mean[i]) * weight;
            n[i] += weight;
        }
        if (std::isfinite(max_in[i]) && (!std::isfinite(max[i]) || max_in[i] > max[i])) max[i] = max_in[i];
    }
}

AggRow Trace::Acc::row(std::int64_t bucket_ms) const noexcept {
    AggRow r;
    r.ts_ms = bucket * bucket_ms;
    r.count = samples;
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        r.mean[i] = n[i] ? static_cast<float>(sum[i] / n[i]) : kNaNf;
        r.max[i] = max[i];
    }
    return r;
}

Trace::Trace() : tier0_(kTier0Rows), tier1_(kTier1Rows), tier2_(kTier2Rows), events_(kMaxEvents) {
    minute_.reset();
    quarter_.reset();
}

void Trace::clear() {
    tier0_.clear();
    tier1_.clear();
    tier2_.clear();
    minute_.reset();
    quarter_.reset();
    events_.clear();
    events_unpersisted_ = 0;
    latest_ = Sample{};
    has_latest_ = false;
}

void Trace::close_minute() {
    if (minute_.bucket < 0 || minute_.samples == 0) {
        minute_.reset();
        return;
    }
    const AggRow r = minute_.row(kTier1BucketMs);
    tier1_.push(r);
    fold_into_quarter(r);
    minute_.reset();
}

void Trace::fold_into_quarter(const AggRow& minute) {
    const std::int64_t qb = floor_div(minute.ts_ms, kTier2BucketMs);
    if (quarter_.bucket >= 0 && quarter_.bucket != qb) close_quarter();
    quarter_.bucket = qb;
    quarter_.add(minute.mean, minute.max, minute.count);
}

void Trace::close_quarter() {
    if (quarter_.bucket >= 0 && quarter_.samples > 0) tier2_.push(quarter_.row(kTier2BucketMs));
    quarter_.reset();
}

void Trace::append(const Sample& s) {
    HiResRow row;
    row.ts_ms = s.ts_ms;
    row.v = values_of(s);
    tier0_.push(row);

    const std::int64_t mb = floor_div(s.ts_ms, kTier1BucketMs);
    if (minute_.bucket >= 0 && minute_.bucket != mb) close_minute();
    minute_.bucket = mb;
    minute_.add(row.v, row.v, 1);

    latest_ = s;  // reuses capacity of latest_.top
    has_latest_ = true;
}

bool Trace::add_event(const EventRecord& e) {
    // After a restart the event-log backfill re-reads events that were already persisted:
    // skip exact duplicates (events arrive rarely, so a scan of the bounded ring is cheap).
    for (std::size_t i = 0; i < events_.size(); ++i) {
        const EventRecord& x = events_[i];
        if (x.ts_ms == e.ts_ms && x.event_id == e.event_id && x.kind == e.kind && x.provider == e.provider &&
            x.subject == e.subject)
            return false;
    }
    events_.push(e);
    events_unpersisted_ = std::min(events_unpersisted_ + 1, events_.size());
    return true;
}

void Trace::add_events(std::span<const EventRecord> events) {
    for (const auto& e : events) add_event(e);
}

std::vector<Point> Trace::history(Metric m, Range r, Agg agg) const {
    const std::size_t mi = index(m);
    std::vector<Point> pts;
    std::int64_t span_ms = 0;
    auto take = [&](std::int64_t ts, float v) {
        if (std::isfinite(v)) pts.push_back({ts, static_cast<double>(v)});
    };
    if (mi >= kMetricCount) return pts;
    switch (r) {
        case Range::Minutes30:
            span_ms = static_cast<std::int64_t>(kTier0Rows) * telemetry::kSampleIntervalMs;
            pts.reserve(tier0_.size());
            for (std::size_t i = 0; i < tier0_.size(); ++i) take(tier0_[i].ts_ms, tier0_[i].v[mi]);
            break;
        case Range::Hours24:
        case Range::Days14: {
            const bool day = r == Range::Hours24;
            const auto& tier = day ? tier1_ : tier2_;
            const Acc& open = day ? minute_ : quarter_;
            const std::int64_t bms = day ? kTier1BucketMs : kTier2BucketMs;
            span_ms = static_cast<std::int64_t>(day ? kTier1Rows : kTier2Rows) * bms;
            pts.reserve(tier.size() + 1);
            for (std::size_t i = 0; i < tier.size(); ++i) {
                const auto& row = tier[i];
                take(row.ts_ms, agg == Agg::Max ? row.max[mi] : row.mean[mi]);
            }
            if (open.bucket >= 0 && open.samples > 0) {
                const AggRow row = open.row(bms);
                take(row.ts_ms, agg == Agg::Max ? row.max[mi] : row.mean[mi]);
            }
            break;
        }
    }
    if (pts.empty()) return pts;
    std::int64_t newest = pts.front().ts_ms;
    for (const auto& p : pts) newest = std::max(newest, p.ts_ms);
    const std::int64_t cutoff = newest - span_ms;
    std::erase_if(pts, [&](const Point& p) { return p.ts_ms <= cutoff; });
    if (pts.size() <= kMaxHistoryPoints) return pts;

    const std::size_t group = (pts.size() + kMaxHistoryPoints - 1) / kMaxHistoryPoints;
    std::vector<Point> out;
    out.reserve(pts.size() / group + 1);
    for (std::size_t i = 0; i < pts.size(); i += group) {
        const std::size_t end = std::min(pts.size(), i + group);
        double acc = agg == Agg::Max ? pts[i].value : 0.0;
        for (std::size_t j = i; j < end; ++j) {
            if (agg == Agg::Max) acc = std::max(acc, pts[j].value);
            else acc += pts[j].value;
        }
        if (agg == Agg::Mean) acc /= static_cast<double>(end - i);
        out.push_back({pts[i].ts_ms, acc});
    }
    return out;
}

std::vector<HiResRow> Trace::window(std::int64_t from_ms, std::int64_t to_ms) const {
    std::vector<HiResRow> out;
    for (std::size_t i = 0; i < tier0_.size(); ++i) {
        const auto& r = tier0_[i];
        if (r.ts_ms >= from_ms && r.ts_ms <= to_ms) out.push_back(r);
    }
    return out;
}

std::vector<EventRecord> Trace::events(std::size_t limit) const {
    // Insertion order is not time order (a backfill after restart can add older events after
    // newer persisted ones), so sort newest first by timestamp; ties keep newest-inserted first.
    std::vector<EventRecord> out;
    out.reserve(events_.size());
    for (std::size_t i = 0; i < events_.size(); ++i) out.push_back(events_[events_.size() - 1 - i]);
    std::stable_sort(out.begin(), out.end(),
                     [](const EventRecord& a, const EventRecord& b) { return a.ts_ms > b.ts_ms; });
    if (limit != 0 && out.size() > limit) out.resize(limit);
    return out;
}

// ---------------------------------------------------------------------------------------------
// trace.bin layout (all integers little-endian):
//   header : "OSTR" | u32 version (1) | u32 metric_count | u32 block_count | u32 crc32(previous 16 bytes)
//   block  : u32 type | u32 rows | u32 payload_len | u32 crc32(type|rows|payload_len|payload) | payload
//   type 1 : metric names, '\n'-separated (column order of the tier blocks)
//   type 10: tier 0 rows: i64 ts | f32 x metric_count
//   type 11/12: tier 1/2 rows: i64 ts | u32 count | f32 mean x metric_count | f32 max x metric_count
// Columns are mapped by name on load, so metrics can be added or removed in later versions.
std::string Trace::serialize() const {
    std::string out;
    out.reserve(kHeaderSize + 4 * kBlockHeaderSize + tier0_.size() * (8 + 4 * kMetricCount) +
                (tier1_.size() + tier2_.size()) * (12 + 8 * kMetricCount) + 512);
    out.append(kMagic, 4);
    put_u32(out, kVersion);
    put_u32(out, static_cast<std::uint32_t>(kMetricCount));
    put_u32(out, 4);
    put_u32(out, crc32(std::string_view(out.data(), 16)));

    std::string payload;
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        if (i) payload.push_back('\n');
        payload += kMetrics[i].name;
    }
    put_block(out, kBlockNames, static_cast<std::uint32_t>(kMetricCount), payload);

    payload.clear();
    for (std::size_t i = 0; i < tier0_.size(); ++i) {
        put_i64(payload, tier0_[i].ts_ms);
        for (float f : tier0_[i].v) put_f32(payload, f);
    }
    put_block(out, kBlockTier0, static_cast<std::uint32_t>(tier0_.size()), payload);

    auto agg_block = [&](std::uint32_t type, const RingBuffer<AggRow>& tier) {
        payload.clear();
        for (std::size_t i = 0; i < tier.size(); ++i) {
            const auto& r = tier[i];
            put_i64(payload, r.ts_ms);
            put_u32(payload, r.count);
            for (float f : r.mean) put_f32(payload, f);
            for (float f : r.max) put_f32(payload, f);
        }
        put_block(out, type, static_cast<std::uint32_t>(tier.size()), payload);
    };
    agg_block(kBlockTier1, tier1_);
    agg_block(kBlockTier2, tier2_);
    return out;
}

LoadReport Trace::deserialize(std::string_view bytes) {
    LoadReport rep;
    rep.found = true;
    tier0_.clear();
    tier1_.clear();
    tier2_.clear();
    minute_.reset();
    quarter_.reset();

    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() < kHeaderSize || std::memcmp(p, kMagic, 4) != 0 ||
        crc32(bytes.substr(0, 16)) != get_u32(p + 16) || get_u32(p + 4) != kVersion) {
        rep.corrupt = 1;
        return rep;
    }
    std::size_t file_metrics = get_u32(p + 8);
    const std::uint32_t block_count = get_u32(p + 12);
    if (file_metrics == 0 || file_metrics > kMaxFileMetrics) {
        rep.corrupt = 1;
        return rep;
    }
    // Column map: file column -> our metric index (or -1). Default: identity when counts match.
    std::vector<int> colmap(file_metrics, -1);
    bool have_map = false;
    if (file_metrics == kMetricCount) {
        for (std::size_t i = 0; i < kMetricCount; ++i) colmap[i] = static_cast<int>(i);
        have_map = true;
    }

    std::size_t pos = kHeaderSize;
    for (std::uint32_t b = 0; b < block_count; ++b) {
        if (pos == bytes.size()) {
            ++rep.corrupt;  // fewer blocks than announced (truncated at a block boundary)
            break;
        }
        if (bytes.size() - pos < kBlockHeaderSize) {
            ++rep.corrupt;
            break;
        }
        const std::uint32_t type = get_u32(p + pos);
        const std::uint32_t rows = get_u32(p + pos + 4);
        const std::uint32_t len = get_u32(p + pos + 8);
        const std::uint32_t crc = get_u32(p + pos + 12);
        if (len > bytes.size() - pos - kBlockHeaderSize) {
            ++rep.corrupt;  // truncated
            break;
        }
        std::uint32_t c = crc32(bytes.substr(pos, 12));
        const std::string_view payload = bytes.substr(pos + kBlockHeaderSize, len);
        c = crc32(payload, c);
        pos += kBlockHeaderSize + len;
        if (c != crc) {
            ++rep.corrupt;
            continue;
        }
        const auto* q = reinterpret_cast<const unsigned char*>(payload.data());
        if (type == kBlockNames) {
            std::vector<int> m(file_metrics, -1);
            std::size_t col = 0;
            bool ok = true;
            for_each_line(payload, [&](std::string_view name) {
                if (col >= file_metrics) {
                    ok = false;
                    return;
                }
                if (auto mm = metric_from_name(name)) m[col] = static_cast<int>(index(*mm));
                ++col;
            });
            if (!ok || col != file_metrics || rows != file_metrics) {
                ++rep.corrupt;
                continue;
            }
            colmap = std::move(m);
            have_map = true;
        } else if (type == kBlockTier0 || type == kBlockTier1 || type == kBlockTier2) {
            const bool hires = type == kBlockTier0;
            const std::size_t row_size = hires ? 8 + 4 * file_metrics : 12 + 8 * file_metrics;
            if (!have_map || static_cast<std::uint64_t>(rows) * row_size != len) {
                ++rep.corrupt;
                continue;
            }
            for (std::uint32_t r = 0; r < rows; ++r) {
                const unsigned char* rp = q + static_cast<std::size_t>(r) * row_size;
                if (hires) {
                    HiResRow row;
                    row.ts_ms = get_i64(rp);
                    row.v.fill(kNaNf);
                    for (std::size_t col = 0; col < file_metrics; ++col) {
                        if (colmap[col] >= 0) row.v[static_cast<std::size_t>(colmap[col])] = get_f32(rp + 8 + 4 * col);
                    }
                    tier0_.push(row);
                } else {
                    AggRow row;
                    row.ts_ms = get_i64(rp);
                    row.count = get_u32(rp + 8);
                    row.mean.fill(kNaNf);
                    row.max.fill(kNaNf);
                    for (std::size_t col = 0; col < file_metrics; ++col) {
                        if (colmap[col] < 0) continue;
                        const auto dst = static_cast<std::size_t>(colmap[col]);
                        row.mean[dst] = get_f32(rp + 12 + 4 * col);
                        row.max[dst] = get_f32(rp + 12 + 4 * (file_metrics + col));
                    }
                    (type == kBlockTier1 ? tier1_ : tier2_).push(row);
                }
                ++rep.loaded;
            }
        }
        // Unknown block types with a valid CRC are skipped (forward compatibility).
    }
    if (pos < bytes.size() && rep.corrupt == 0) {
        // Trailing bytes after the announced blocks: ignore but report.
        ++rep.corrupt;
    }
    return rep;
}

bool Trace::save(FileIO& io, const std::string& path) const { return io.write_atomic(path, serialize()); }

LoadReport Trace::load(FileIO& io, const std::string& path) {
    std::string bytes;
    if (!io.read_all(path, bytes, kMaxTraceFile)) return {};
    return deserialize(bytes);
}

bool Trace::persist_events(FileIO& io, const std::string& path) {
    const std::size_t pending = std::min(events_unpersisted_, events_.size());
    if (!event_file_known_ || event_file_lines_ + pending > 2 * kMaxEvents) {
        std::string all;
        for (std::size_t i = 0; i < events_.size(); ++i) all += jsonl_seal(event_to_json(events_[i]));
        if (!io.write_atomic(path, all)) return false;
        event_file_known_ = true;
        event_file_lines_ = events_.size();
        events_unpersisted_ = 0;
        return true;
    }
    if (pending == 0) return true;
    std::string lines;
    for (std::size_t i = events_.size() - pending; i < events_.size(); ++i) {
        lines += jsonl_seal(event_to_json(events_[i]));
    }
    if (!io.append(path, lines)) return false;
    event_file_lines_ += pending;
    events_unpersisted_ = 0;
    return true;
}

LoadReport Trace::load_events(FileIO& io, const std::string& path) {
    LoadReport rep;
    std::string text;
    if (!io.read_all(path, text, kMaxEventsFile)) {
        event_file_known_ = false;  // unknown state: the next persist rewrites the file
        return rep;
    }
    rep.found = true;
    std::size_t lines = 0;
    for_each_line(text, [&](std::string_view line) {
        if (trim_line(line).empty()) return;
        ++lines;
        auto obj = jsonl_open(line);
        std::optional<EventRecord> e;
        if (obj) e = event_from_json(*obj);
        if (!e) {
            ++rep.corrupt;
            return;
        }
        events_.push(std::move(*e));
        ++rep.loaded;
    });
    events_unpersisted_ = 0;
    event_file_known_ = true;
    event_file_lines_ = lines;
    return rep;
}

}  // namespace opensweep::trace
