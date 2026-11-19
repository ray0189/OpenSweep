// EventCollector: stability-relevant Windows Event Log records (Windows only, read-only).
//
// One pull-model subscription (EvtSubscribe with a signal event, drained with EvtNext) over a
// structured XML QueryList with one <Select> per provider, limited to the provider/event-id
// pairs listed for each telemetry::EventKind:
//   System:      Service Control Manager 7023/7024/7026/7031/7034, Kernel-Power 41/86/125,
//                EventLog 6008, WER-SystemErrorReporting (BugCheck) 1001, WHEA-Logger (levels 1-3),
//                Kernel-PnP 219, Display 4101, disk 7/11/51/153, Ntfs 55/98, stornvme/storahci 129,
//                WindowsUpdateClient 19/20/43, Kernel-Processor-Power 37 (firmware frequency limit),
//                Resource-Exhaustion-Detector 2004
//   Application: Application Error 1000, Application Hang 1002, Windows Error Reporting 1001
//   Microsoft-Windows-Diagnostics-Performance/Operational: 100 (boot duration)
// The first subscription starts at the oldest record within the last 24 h (backfill, newest
// `backfill_cap` kept); later (re)subscriptions resume after an in-memory bookmark. If the
// Event Log service is unavailable or the subscription breaks, poll() returns false and the
// collector retries with back-off (5 s doubling to 5 min). Missing channels are tolerated
// (EvtSubscribeTolerateQueryErrors).
//
// Privacy: no event text is rendered (no EvtFormatMessage). `message` is built by OpenSweep from
// the kind + `subject`; `subject` is a short name taken from known EventData fields (application
// image name, service name, driver name, disk device, update title), truncated and stripped of
// directory paths and control characters.
//
// ETW (real-time DPC/ISR per-driver attribution, Part 9) is NOT implemented here; deferred.
// Threading: not thread-safe; call poll() from the engine thread only.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "telemetry.hpp"

namespace opensweep::win {

class EventCollector {
public:
    struct Options {
        std::int64_t backfill_ms = 24LL * 60 * 60 * 1000;
        std::size_t backfill_cap = 500;    // newest N kept from the initial backfill
        std::size_t max_per_poll = 256;    // bound on records returned by one poll() after backfill
        std::size_t max_backfill_reads = 20000;  // hard bound on events read during backfill
    };

    EventCollector();
    explicit EventCollector(Options options);
    ~EventCollector();
    EventCollector(const EventCollector&) = delete;
    EventCollector& operator=(const EventCollector&) = delete;

    // Appends new records (oldest first) to `out`. Returns false if the event log is currently
    // unavailable (a retry is scheduled; records read before a failure are still appended); true
    // otherwise, even if nothing new arrived.
    bool poll(std::vector<telemetry::EventRecord>& out);

    bool subscribed() const noexcept;
    bool backfill_done() const noexcept;
    std::uint64_t records_dropped() const noexcept;  // backfill records beyond backfill_cap

    // Pure mapping used by poll(); exposed for tests. `wer_event_name` is the WER "Event Name"
    // (e.g. APPCRASH) for Windows Error Reporting 1001, empty otherwise. Returns false if the
    // event should not be recorded.
    static bool classify(std::string_view provider, std::uint32_t event_id, std::string_view wer_event_name,
                         telemetry::EventKind& kind);
    // Privacy-safe subject cleanup: strips directories (if `strip_path`), control characters,
    // trims and truncates to `max_chars` bytes on a UTF-8 boundary.
    static std::string sanitize_subject(std::string_view raw, bool strip_path, std::size_t max_chars = 96);
    // The structured query (exposed for diagnostics/tests). `with_timediff` adds the 24 h filter.
    static std::wstring build_query(std::int64_t backfill_ms, bool with_timediff);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace opensweep::win
