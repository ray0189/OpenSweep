// SweepTrace: bounded flight recorder for system metrics and stability events (Phase 2, Part 6).
//
// Memory is fixed at construction:
//   tier 0 ("30m"):  one row per sample (2 s)          x  900 rows  (values)
//   tier 1 ("24h"):  one row per wall-clock minute     x 1440 rows  (mean + max per metric)
//   tier 2 ("14d"):  one row per 15 wall-clock minutes x 1344 rows  (mean + max per metric)
//   events:          newest 2 000 EventRecords
//   latest:          the most recent Sample (incl. its bounded `top` process list)
// Values are stored as float (charts and incident windows do not need more precision); detectors
// work on the live Sample, not on the trace.
//
// Aggregation: a minute bucket is closed when the first sample of a later minute arrives (also when
// the clock moves backwards); the minute row's mean is over the finite samples of that minute, its
// max the largest of them. Closed minute rows are folded into the open 15-minute bucket the same way
// (mean weighted by sample count). Open buckets are visible through history() as the last point but
// are not persisted (a restart loses at most the open minute / quarter hour).
//
// Persistence (`trace.bin`): little-endian, versioned, CRC32 per block, see trace.cpp. Loading never
// throws on bad input: valid blocks are kept and every corrupt/truncated block is counted.
// Events are persisted as JSONL with a per-line "crc" field (see jsonl_seal/jsonl_open).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"
#include "ringbuffer.hpp"
#include "telemetry.hpp"

namespace opensweep::trace {

using telemetry::EventKind;
using telemetry::EventRecord;
using telemetry::Sample;

// ---------------------------------------------------------------------------------------------
// Metric catalogue: the numeric fields of telemetry::Sample (+ derived commitPercent).
enum class Metric : std::uint8_t {
    CpuPercent,
    CpuQueueLength,
    ContextSwitchesPerSec,
    DpcPercent,
    InterruptPercent,
    MemTotal,
    MemAvail,
    MemLoadPercent,
    CommitTotal,
    CommitLimit,
    CommitPercent,  // derived: commit_total / commit_limit * 100 (NaN when the limit is 0)
    SystemCache,
    KernelPaged,
    KernelNonpaged,
    CompressedBytes,
    HardFaultsPerSec,
    PageFaultsPerSec,
    DiskReadLatencyMs,
    DiskWriteLatencyMs,
    DiskQueueLength,
    DiskBytesPerSec,
    CpuFreqPercent,
    Processes,
    Threads,
    Handles,
};
inline constexpr std::size_t kMetricCount = 25;

struct MetricInfo {
    Metric metric;
    const char* name;  // camelCase, as used by IPC `history` and `baselines`
    const char* unit;  // "%", "bytes", "ms", "/s", "B/s", "count", "threads"
};

std::span<const MetricInfo> metrics() noexcept;  // kMetricCount entries, in enum order
const MetricInfo& info(Metric m) noexcept;
std::optional<Metric> metric_from_name(std::string_view name) noexcept;
double metric_value(const Sample& s, Metric m) noexcept;
inline std::size_t index(Metric m) noexcept { return static_cast<std::size_t>(m); }

using Values = std::array<float, kMetricCount>;

// ---------------------------------------------------------------------------------------------
// Rows and ranges.
struct HiResRow {   // tier 0
    std::int64_t ts_ms = 0;
    Values v{};
};
struct AggRow {     // tiers 1 and 2; ts_ms is the bucket start
    std::int64_t ts_ms = 0;
    std::uint32_t count = 0;  // number of 2 s samples folded in
    Values mean{};
    Values max{};
};

enum class Range { Minutes30, Hours24, Days14 };
std::optional<Range> range_from_string(std::string_view s) noexcept;  // "30m" | "24h" | "14d"
const char* to_string(Range r) noexcept;
enum class Agg { Mean, Max };

struct Point {
    std::int64_t ts_ms = 0;
    double value = 0.0;
};
inline constexpr std::size_t kMaxHistoryPoints = 900;

inline constexpr std::size_t kTier0Rows = 900;    // 2 s x 30 min
inline constexpr std::size_t kTier1Rows = 1440;   // 1 min x 24 h
inline constexpr std::size_t kTier2Rows = 1344;   // 15 min x 14 d
inline constexpr std::int64_t kTier1BucketMs = 60'000;
inline constexpr std::int64_t kTier2BucketMs = 15 * 60'000;
inline constexpr std::size_t kMaxEvents = 2000;

// ---------------------------------------------------------------------------------------------
// Pluggable file access (atomic writes for snapshots, appends for JSONL).
class FileIO {
public:
    virtual ~FileIO() = default;
    // Reads the whole file. Returns false if missing/unreadable or larger than max_bytes.
    virtual bool read_all(const std::string& path, std::string& out, std::size_t max_bytes) = 0;
    // Writes `path + ".tmp"`, flushes, then renames over `path`.
    virtual bool write_atomic(const std::string& path, std::string_view data) = 0;
    // Appends and flushes.
    virtual bool append(const std::string& path, std::string_view data) = 0;
};

// std::filesystem / std::fstream implementation. Note: std streams can only flush to the OS, they
// cannot fsync; the Windows build may substitute an implementation using FlushFileBuffers.
// std::filesystem::rename replaces an existing target on both POSIX and Windows (MSVC STL).
class StdFileIO final : public FileIO {
public:
    bool read_all(const std::string& path, std::string& out, std::size_t max_bytes) override;
    bool write_atomic(const std::string& path, std::string_view data) override;
    bool append(const std::string& path, std::string_view data) override;
};

// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), as used by zip/PNG.
std::uint32_t crc32(std::string_view data, std::uint32_t seed = 0) noexcept;

// JSONL line sealing. jsonl_seal(obj) serialises `obj` (which must be an object) and appends a
// "crc" member holding the 8-hex-digit CRC32 of the serialisation *without* that member, followed
// by '\n'. jsonl_open() verifies such a line (with or without the trailing newline) and returns the
// object without the "crc" member, or nullopt if the line is corrupt.
std::string jsonl_seal(const json::Value& obj);
std::optional<json::Value> jsonl_open(std::string_view line);

struct LoadReport {
    bool found = false;        // the file existed and was read
    std::size_t loaded = 0;    // rows / records restored
    std::size_t corrupt = 0;   // corrupt blocks / lines skipped (or 1 for an unusable header)
};

// Event (de)serialisation shared with other stores.
json::Value event_to_json(const EventRecord& e);
std::optional<EventRecord> event_from_json(const json::Value& v);
std::optional<EventKind> event_kind_from_string(std::string_view s) noexcept;

// ---------------------------------------------------------------------------------------------
class Trace {
public:
    Trace();

    // Appends a sample to tier 0, folds it into the open aggregation buckets and keeps it as latest.
    void append(const Sample& s);
    void add_event(const EventRecord& e);
    void add_events(std::span<const EventRecord> events);

    const Sample* latest() const noexcept { return has_latest_ ? &latest_ : nullptr; }
    const std::vector<telemetry::ProcessSample>& top() const noexcept { return latest_.top; }

    // <= kMaxHistoryPoints points, oldest first, covering `range` back from the newest point.
    // Non-finite values are skipped. Larger series are downsampled by averaging (Agg::Mean) or
    // taking the maximum (Agg::Max) of consecutive groups.
    std::vector<Point> history(Metric m, Range r, Agg agg = Agg::Mean) const;

    // Copy of the high-resolution rows with from_ms <= ts <= to_ms (incident capture), oldest first.
    std::vector<HiResRow> window(std::int64_t from_ms, std::int64_t to_ms) const;

    // Newest first; limit 0 = all.
    std::vector<EventRecord> events(std::size_t limit = 0) const;
    std::size_t event_count() const noexcept { return events_.size(); }

    const RingBuffer<HiResRow>& tier0() const noexcept { return tier0_; }
    const RingBuffer<AggRow>& tier1() const noexcept { return tier1_; }
    const RingBuffer<AggRow>& tier2() const noexcept { return tier2_; }

    // Binary snapshot of the three tiers (events are kept in JSONL, see below).
    std::string serialize() const;
    LoadReport deserialize(std::string_view bytes);  // replaces tier contents with what is valid
    bool save(FileIO& io, const std::string& path) const;
    LoadReport load(FileIO& io, const std::string& path);

    // Events JSONL: appends events added since the last persist; when the file has grown beyond
    // 2 x kMaxEvents lines it is rewritten atomically with the current (bounded) contents.
    bool persist_events(FileIO& io, const std::string& path);
    LoadReport load_events(FileIO& io, const std::string& path);

    void clear();

private:
    void close_minute();
    void fold_into_quarter(const AggRow& minute);
    void close_quarter();

    struct Acc {
        std::int64_t bucket = -1;  // bucket index (ts / bucket_ms), -1 = empty
        std::uint32_t samples = 0;
        std::array<double, kMetricCount> sum{};
        std::array<std::uint32_t, kMetricCount> n{};
        Values max{};
        void reset() noexcept;
        void add(const Values& mean, const Values& max_in, std::uint32_t weight) noexcept;
        AggRow row(std::int64_t bucket_ms) const noexcept;
    };

    RingBuffer<HiResRow> tier0_;
    RingBuffer<AggRow> tier1_;
    RingBuffer<AggRow> tier2_;
    Acc minute_;
    Acc quarter_;
    RingBuffer<EventRecord> events_;
    std::size_t events_unpersisted_ = 0;
    std::size_t event_file_lines_ = 0;
    bool event_file_known_ = false;
    Sample latest_;
    bool has_latest_ = false;
};

}  // namespace opensweep::trace
