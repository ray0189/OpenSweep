#include "events.hpp"

#include <windows.h>
#include <winevt.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>

#include "log.hpp"
#include "process_info.hpp"

namespace opensweep::win {

namespace {

using telemetry::EventKind;
using telemetry::EventRecord;

struct SelectDef {
    const wchar_t* channel;
    const wchar_t* provider;
    const wchar_t* filter;  // XPath fragment inside System[...]
};

constexpr const wchar_t* kSystem = L"System";
constexpr const wchar_t* kApplication = L"Application";
constexpr const wchar_t* kDiagPerf = L"Microsoft-Windows-Diagnostics-Performance/Operational";

// Kept small per <Select> (the event-log XPath subset limits expression size per select).
const SelectDef kSelects[] = {
    {kSystem, L"Service Control Manager",
     L"(EventID=7023 or EventID=7024 or EventID=7026 or EventID=7031 or EventID=7034)"},
    {kSystem, L"Microsoft-Windows-Kernel-Power", L"(EventID=41 or EventID=86 or EventID=125)"},
    {kSystem, L"EventLog", L"(EventID=6008)"},
    {kSystem, L"Microsoft-Windows-WER-SystemErrorReporting", L"(EventID=1001)"},
    {kSystem, L"Microsoft-Windows-WHEA-Logger", L"(Level=1 or Level=2 or Level=3)"},
    {kSystem, L"Microsoft-Windows-Kernel-PnP", L"(EventID=219)"},
    {kSystem, L"Display", L"(EventID=4101)"},
    {kSystem, L"disk", L"(EventID=7 or EventID=11 or EventID=51 or EventID=153)"},
    {kSystem, L"Ntfs", L"(EventID=55 or EventID=98)"},
    {kSystem, L"Microsoft-Windows-Ntfs", L"(EventID=55 or EventID=98)"},
    {kSystem, L"stornvme", L"(EventID=129)"},
    {kSystem, L"storahci", L"(EventID=129)"},
    {kSystem, L"Microsoft-Windows-WindowsUpdateClient", L"(EventID=19 or EventID=20 or EventID=43)"},
    {kSystem, L"Microsoft-Windows-Kernel-Processor-Power", L"(EventID=37)"},
    {kSystem, L"Microsoft-Windows-Resource-Exhaustion-Detector", L"(EventID=2004)"},
    {kApplication, L"Application Error", L"(EventID=1000)"},
    {kApplication, L"Application Hang", L"(EventID=1002)"},
    {kApplication, L"Windows Error Reporting", L"(EventID=1001)"},
    {kDiagPerf, L"Microsoft-Windows-Diagnostics-Performance", L"(EventID=100)"},
};

// EventData fields used for `subject` / `value`. Named fields first; positional ones live in a
// second render context so that a rejection of positional paths cannot disable the named ones.
enum NamedField { kAppName, kParam1, kDriverName, kUpdateTitle, kBootTime, kBugcheckCode, kDriveName, kNamedCount };
const wchar_t* const kNamedPaths[kNamedCount] = {
    L"Event/EventData/Data[@Name='AppName']",     L"Event/EventData/Data[@Name='param1']",
    L"Event/EventData/Data[@Name='DriverName']",  L"Event/EventData/Data[@Name='updateTitle']",
    L"Event/EventData/Data[@Name='BootTime']",    L"Event/EventData/Data[@Name='BugcheckCode']",
    L"Event/EventData/Data[@Name='DriveName']",
};
enum PosField { kData1, kData3, kData6, kPosCount };
const wchar_t* const kPosPaths[kPosCount] = {
    L"Event/EventData/Data[1]",
    L"Event/EventData/Data[3]",
    L"Event/EventData/Data[6]",
};

constexpr DWORD kBatch = 64;
constexpr ULONGLONG kRetryMinMs = 5000;
constexpr ULONGLONG kRetryMaxMs = 5 * 60 * 1000;
constexpr std::size_t kMaxMessage = 200;

std::int64_t now_unix_ms() noexcept {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    return procinfo::filetime_to_unix_ms(procinfo::filetime_u64(ft));
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::string variant_string(const EVT_VARIANT& v) {
    if (v.Type & EVT_VARIANT_TYPE_ARRAY) return {};
    switch (v.Type & EVT_VARIANT_TYPE_MASK) {
        case EvtVarTypeString:
            return v.StringVal ? log::narrow(v.StringVal) : std::string();
        case EvtVarTypeAnsiString:
            return v.AnsiStringVal ? std::string(v.AnsiStringVal) : std::string();
        case EvtVarTypeByte: return std::to_string(v.ByteVal);
        case EvtVarTypeSByte: return std::to_string(v.SByteVal);
        case EvtVarTypeInt16: return std::to_string(v.Int16Val);
        case EvtVarTypeUInt16: return std::to_string(v.UInt16Val);
        case EvtVarTypeInt32: return std::to_string(v.Int32Val);
        case EvtVarTypeUInt32: return std::to_string(v.UInt32Val);
        case EvtVarTypeInt64: return std::to_string(v.Int64Val);
        case EvtVarTypeUInt64: return std::to_string(v.UInt64Val);
        case EvtVarTypeHexInt32: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(v.UInt32Val));
            return buf;
        }
        case EvtVarTypeHexInt64: {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(v.UInt64Val));
            return buf;
        }
        default:
            return {};
    }
}

bool variant_number(const EVT_VARIANT& v, double& out) {
    if (v.Type & EVT_VARIANT_TYPE_ARRAY) return false;
    switch (v.Type & EVT_VARIANT_TYPE_MASK) {
        case EvtVarTypeByte: out = v.ByteVal; return true;
        case EvtVarTypeUInt16: out = v.UInt16Val; return true;
        case EvtVarTypeUInt32:
        case EvtVarTypeHexInt32: out = v.UInt32Val; return true;
        case EvtVarTypeUInt64:
        case EvtVarTypeHexInt64: out = static_cast<double>(v.UInt64Val); return true;
        case EvtVarTypeInt16: out = v.Int16Val; return true;
        case EvtVarTypeInt32: out = v.Int32Val; return true;
        case EvtVarTypeInt64: out = static_cast<double>(v.Int64Val); return true;
        case EvtVarTypeDouble: out = v.DoubleVal; return true;
        case EvtVarTypeString: {
            if (!v.StringVal) return false;
            wchar_t* end = nullptr;
            const double d = std::wcstod(v.StringVal, &end);
            if (end == v.StringVal) return false;
            out = d;
            return true;
        }
        default:
            return false;
    }
}

// Renders `ctx` values for `event` into `buf`. Returns the number of values (0 on failure).
DWORD render_values(EVT_HANDLE ctx, EVT_HANDLE event, std::vector<EVT_VARIANT>& buf) {
    if (!ctx) return 0;
    DWORD used = 0, count = 0;
    DWORD bytes = static_cast<DWORD>(buf.size() * sizeof(EVT_VARIANT));
    if (EvtRender(ctx, event, EvtRenderEventValues, bytes, buf.data(), &used, &count)) return count;
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || used == 0 || used > 1024 * 1024) return 0;
    buf.resize(used / sizeof(EVT_VARIANT) + 1);
    bytes = static_cast<DWORD>(buf.size() * sizeof(EVT_VARIANT));
    if (EvtRender(ctx, event, EvtRenderEventValues, bytes, buf.data(), &used, &count)) return count;
    return 0;
}

std::string first_token(std::string_view s) {
    const auto pos = s.find_first_of(" \t\r\n(");
    return std::string(s.substr(0, pos));
}

std::string truncate_utf8(std::string s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;  // not inside a code point
    s.resize(cut);
    return s;
}

}  // namespace

// ------------------------------------------------------------------------------------------------

bool EventCollector::classify(std::string_view provider, std::uint32_t id, std::string_view wer_event_name,
                              EventKind& kind) {
    auto is = [&](std::string_view p) { return ieq(provider, p); };
    if (is("Application Error") && id == 1000) { kind = EventKind::AppCrash; return true; }
    if (is("Application Hang") && id == 1002) { kind = EventKind::AppHang; return true; }
    if (is("Windows Error Reporting") && id == 1001) {
        if (ieq(wer_event_name, "APPCRASH") || ieq(wer_event_name, "BEX") || ieq(wer_event_name, "BEX64") ||
            ieq(wer_event_name, "MoAppCrash") || ieq(wer_event_name, "CLR20r3")) {
            kind = EventKind::AppCrash;
            return true;
        }
        if (ieq(wer_event_name, "AppHangB1") || ieq(wer_event_name, "AppHangXProcB1") ||
            ieq(wer_event_name, "MoAppHang")) {
            kind = EventKind::AppHang;
            return true;
        }
        return false;  // other WER reports (store, live kernel, blue screen duplicate) are not recorded
    }
    if (is("Service Control Manager")) {
        if (id == 7031 || id == 7034 || id == 7023 || id == 7024) { kind = EventKind::ServiceFailure; return true; }
        if (id == 7026) { kind = EventKind::DriverFailure; return true; }
        return false;
    }
    if (is("Microsoft-Windows-Kernel-Power")) {
        if (id == 41) { kind = EventKind::UnexpectedShutdown; return true; }
        if (id == 86 || id == 125) { kind = EventKind::Thermal; return true; }
        return false;
    }
    if (is("EventLog") && id == 6008) { kind = EventKind::UnexpectedShutdown; return true; }
    if (is("Microsoft-Windows-WER-SystemErrorReporting") && id == 1001) { kind = EventKind::Bugcheck; return true; }
    if (is("Microsoft-Windows-WHEA-Logger")) { kind = EventKind::Whea; return true; }
    if (is("Microsoft-Windows-Kernel-PnP") && id == 219) { kind = EventKind::DriverFailure; return true; }
    if (is("Display") && id == 4101) { kind = EventKind::GpuTimeout; return true; }
    if (is("disk") && (id == 7 || id == 11 || id == 51 || id == 153)) { kind = EventKind::DiskError; return true; }
    if ((is("Ntfs") || is("Microsoft-Windows-Ntfs")) && (id == 55 || id == 98)) {
        kind = EventKind::DiskError;
        return true;
    }
    if ((is("stornvme") || is("storahci")) && id == 129) { kind = EventKind::DiskError; return true; }
    if (is("Microsoft-Windows-WindowsUpdateClient") && (id == 19 || id == 20 || id == 43)) {
        kind = EventKind::WindowsUpdate;
        return true;
    }
    if (is("Microsoft-Windows-Kernel-Processor-Power") && id == 37) { kind = EventKind::Thermal; return true; }
    if (is("Microsoft-Windows-Resource-Exhaustion-Detector") && id == 2004) {
        kind = EventKind::ResourceExhaustion;
        return true;
    }
    if (is("Microsoft-Windows-Diagnostics-Performance") && id == 100) { kind = EventKind::BootPerformance; return true; }
    return false;
}

std::string EventCollector::sanitize_subject(std::string_view raw, bool strip_path, std::size_t max_chars) {
    std::string s;
    s.reserve(std::min(raw.size(), max_chars + 8));
    for (char c : raw) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            if (!s.empty() && s.back() != ' ') s.push_back(' ');  // newlines in driver lists etc.
            continue;
        }
        s.push_back(c);
    }
    if (strip_path) {
        const auto pos = s.find_last_of("\\/");
        if (pos != std::string::npos) s.erase(0, pos + 1);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    std::size_t lead = 0;
    while (lead < s.size() && s[lead] == ' ') ++lead;
    s.erase(0, lead);
    return truncate_utf8(std::move(s), max_chars);
}

std::wstring EventCollector::build_query(std::int64_t backfill_ms, bool with_timediff) {
    std::wstring time_clause;
    if (with_timediff && backfill_ms > 0)
        time_clause = L" and TimeCreated[timediff(@SystemTime) &lt;= " + std::to_wstring(backfill_ms) + L"]";
    std::wstring q = L"<QueryList><Query Id=\"0\">";
    for (const SelectDef& s : kSelects) {
        q += L"<Select Path=\"";
        q += s.channel;
        q += L"\">*[System[Provider[@Name='";
        q += s.provider;
        q += L"'] and ";
        q += s.filter;
        q += time_clause;
        q += L"]]</Select>";
    }
    q += L"</Query></QueryList>";
    return q;
}

// ------------------------------------------------------------------------------------------------

struct EventCollector::Impl {
    Options opt;
    EVT_HANDLE sys_ctx = nullptr;
    EVT_HANDLE named_ctx = nullptr;
    EVT_HANDLE pos_ctx = nullptr;
    EVT_HANDLE sub = nullptr;
    EVT_HANDLE bookmark = nullptr;
    HANDLE signal = nullptr;
    bool bookmark_used = false;
    bool use_timediff = true;
    bool backfilling = false;
    bool backfill_done = false;
    std::size_t backfill_reads = 0;
    std::vector<EventRecord> ring;  // backfill candidates; trimmed to the newest backfill_cap by time
    std::uint64_t dropped = 0;
    ULONGLONG next_retry = 0;
    ULONGLONG backoff = kRetryMinMs;
    bool warned_unavailable = false;
    std::vector<EVT_VARIANT> sys_buf, named_buf, pos_buf;
    EVT_HANDLE batch[kBatch] = {};

    explicit Impl(Options o) : opt(std::move(o)) {
        if (opt.backfill_cap == 0) opt.backfill_cap = 1;
        if (opt.max_per_poll == 0) opt.max_per_poll = 1;
        sys_buf.resize(EvtSystemPropertyIdEND + 4);
        named_buf.resize(kNamedCount + 4);
        pos_buf.resize(kPosCount + 4);
        signal = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        create_contexts();
    }

    ~Impl() {
        close_sub();
        if (bookmark) EvtClose(bookmark);
        if (pos_ctx) EvtClose(pos_ctx);
        if (named_ctx) EvtClose(named_ctx);
        if (sys_ctx) EvtClose(sys_ctx);
        if (signal) CloseHandle(signal);
    }

    void create_contexts() {
        if (!sys_ctx) {
            sys_ctx = EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem);
            if (!sys_ctx) log::warn("EvtCreateRenderContext(system) failed: " + log::win32_error(GetLastError()));
        }
        if (!named_ctx) {
            named_ctx = EvtCreateRenderContext(kNamedCount, const_cast<LPCWSTR*>(kNamedPaths), EvtRenderContextValues);
            if (!named_ctx)
                log::warn("EvtCreateRenderContext(named fields) failed: " + log::win32_error(GetLastError()) +
                          "; subjects limited");
        }
        if (!pos_ctx) {
            pos_ctx = EvtCreateRenderContext(kPosCount, const_cast<LPCWSTR*>(kPosPaths), EvtRenderContextValues);
            if (!pos_ctx)
                log::warn("EvtCreateRenderContext(positional fields) failed: " + log::win32_error(GetLastError()) +
                          "; subjects limited");
        }
        if (!bookmark) {
            bookmark = EvtCreateBookmark(nullptr);
            if (!bookmark) log::warn("EvtCreateBookmark failed: " + log::win32_error(GetLastError()));
        }
    }

    void close_sub() {
        if (sub) {
            EvtClose(sub);
            sub = nullptr;
        }
    }

    void schedule_retry() {
        next_retry = GetTickCount64() + backoff;
        backoff = std::min(backoff * 2, kRetryMaxMs);
    }

    bool subscribe() {
        create_contexts();
        if (!sys_ctx || !signal) return false;
        const bool resume = bookmark && bookmark_used;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const std::wstring query = build_query(opt.backfill_ms, use_timediff);
            const DWORD flags = static_cast<DWORD>(resume ? EvtSubscribeStartAfterBookmark : EvtSubscribeStartAtOldestRecord) |
                                static_cast<DWORD>(EvtSubscribeTolerateQueryErrors);
            sub = EvtSubscribe(nullptr, signal, nullptr, query.c_str(), resume ? bookmark : nullptr, nullptr, nullptr,
                               flags);
            if (sub) break;
            const DWORD err = GetLastError();
            if (err == ERROR_EVT_INVALID_QUERY && use_timediff) {
                // Fall back to filtering the 24 h window in code.
                log::warn("event query with timediff rejected; filtering the backfill window in code");
                use_timediff = false;
                continue;
            }
            if (!warned_unavailable) {
                warned_unavailable = true;
                log::warn("EvtSubscribe failed: " + log::win32_error(err) + "; will retry");
            }
            return false;
        }
        if (!sub) return false;
        backfilling = !resume;
        backfill_reads = 0;
        ring.clear();
        backoff = kRetryMinMs;
        if (warned_unavailable) log::info("event log subscription (re)established");
        warned_unavailable = false;
        return true;
    }

    bool to_record(EVT_HANDLE ev, EventRecord& rec) {
        const DWORD n = render_values(sys_ctx, ev, sys_buf);
        if (n < static_cast<DWORD>(EvtSystemPropertyIdEND)) return false;
        const EVT_VARIANT& prov = sys_buf[EvtSystemProviderName];
        const EVT_VARIANT& id = sys_buf[EvtSystemEventID];
        const EVT_VARIANT& lvl = sys_buf[EvtSystemLevel];
        const EVT_VARIANT& tc = sys_buf[EvtSystemTimeCreated];
        if ((prov.Type & EVT_VARIANT_TYPE_MASK) != EvtVarTypeString || !prov.StringVal) return false;
        rec = EventRecord{};
        rec.provider = log::narrow(prov.StringVal);
        if ((id.Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeUInt16) rec.event_id = id.UInt16Val;
        std::uint8_t level = 4;
        if ((lvl.Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeByte) level = lvl.ByteVal;
        rec.level = (level >= 1 && level <= 4) ? level : static_cast<std::uint8_t>(4);
        if ((tc.Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeFileTime) rec.ts_ms = procinfo::filetime_to_unix_ms(tc.FileTimeVal);

        // Event data fields (missing fields render as EvtVarTypeNull).
        const DWORD nn = render_values(named_ctx, ev, named_buf);
        const DWORD np = render_values(pos_ctx, ev, pos_buf);
        auto named = [&](NamedField f) { return nn > static_cast<DWORD>(f) ? variant_string(named_buf[f]) : std::string(); };
        auto pos = [&](PosField f) { return np > static_cast<DWORD>(f) ? variant_string(pos_buf[f]) : std::string(); };
        auto either = [](std::string a, std::string b) { return a.empty() ? b : a; };

        const bool wer = ieq(rec.provider, "Windows Error Reporting");
        EventKind kind = EventKind::Other;
        if (!classify(rec.provider, rec.event_id, wer ? pos(kData3) : std::string(), kind)) return false;
        // Ntfs 98 is informational ("volume is healthy") unless it is a warning/error.
        if (kind == EventKind::DiskError && rec.event_id == 98 && rec.level > 3) return false;
        rec.kind = kind;

        const std::string id_str = std::to_string(rec.event_id);
        std::string msg;
        switch (kind) {
            case EventKind::AppCrash:
                rec.subject = sanitize_subject(wer ? pos(kData6) : either(named(kAppName), pos(kData1)), true);
                msg = "Application crashed: " + rec.subject;
                break;
            case EventKind::AppHang:
                rec.subject = sanitize_subject(wer ? pos(kData6) : either(named(kAppName), pos(kData1)), true);
                msg = "Application stopped responding: " + rec.subject;
                break;
            case EventKind::ServiceFailure:
                rec.subject = sanitize_subject(either(named(kParam1), pos(kData1)), false);
                msg = (rec.event_id == 7031 || rec.event_id == 7034 ? "Service terminated unexpectedly: "
                                                                     : "Service stopped with an error: ") +
                      rec.subject;
                break;
            case EventKind::DriverFailure:
                if (rec.event_id == 219) {
                    rec.subject = sanitize_subject(named(kDriverName), true);
                    msg = "Driver failed to load: " + rec.subject;
                } else {
                    rec.subject = sanitize_subject(either(named(kParam1), pos(kData1)), false);
                    msg = "Boot/system-start driver(s) did not load: " + rec.subject;
                }
                break;
            case EventKind::UnexpectedShutdown:
                if (rec.event_id == 41) {
                    double code = 0.0;
                    if (nn > static_cast<DWORD>(kBugcheckCode) && variant_number(named_buf[kBugcheckCode], code)) rec.value = code;
                    msg = "System restarted without a clean shutdown (Kernel-Power 41)";
                } else {
                    msg = "The previous system shutdown was unexpected";
                }
                break;
            case EventKind::Bugcheck:
                rec.subject = sanitize_subject(first_token(either(named(kParam1), pos(kData1))), false, 32);
                msg = "Bugcheck (blue screen) " + rec.subject;
                break;
            case EventKind::Whea:
                msg = "Hardware error reported by WHEA (event " + id_str + ")";
                break;
            case EventKind::GpuTimeout:
                rec.subject = sanitize_subject(pos(kData1), true);
                msg = "Display driver stopped responding and recovered: " + rec.subject;
                break;
            case EventKind::DiskError:
                rec.subject = sanitize_subject(either(named(kDriveName), pos(kData1)), false);
                msg = "Storage error (" + rec.provider + " " + id_str + ") on " + rec.subject;
                break;
            case EventKind::WindowsUpdate:
                rec.subject = sanitize_subject(named(kUpdateTitle), false, 120);
                msg = (rec.event_id == 19 ? "Update installed: "
                                          : rec.event_id == 20 ? "Update installation failed: "
                                                               : "Update installation started: ") +
                      rec.subject;
                break;
            case EventKind::Thermal:
                msg = rec.event_id == 37 ? "Processor speed limited by system firmware"
                                         : "Thermal event (Kernel-Power " + id_str + ")";
                break;
            case EventKind::BootPerformance: {
                double ms = 0.0;
                if (nn > static_cast<DWORD>(kBootTime) && variant_number(named_buf[kBootTime], ms)) rec.value = ms;
                msg = "Boot completed in " + std::to_string(static_cast<long long>(rec.value / 1000.0)) + " s";
                break;
            }
            case EventKind::ResourceExhaustion:
                msg = "Windows detected a low virtual memory condition";
                break;
            default:
                msg = rec.provider + " " + id_str;
                break;
        }
        rec.message = truncate_utf8(std::move(msg), kMaxMessage);
        return true;
    }

    // Keeps the newest backfill_cap records by timestamp (channels are delivered one after the
    // other, so arrival order is not time order).
    void trim_ring() {
        if (ring.size() <= opt.backfill_cap) return;
        const auto keep = static_cast<std::ptrdiff_t>(opt.backfill_cap);
        std::nth_element(ring.begin(), ring.begin() + keep, ring.end(),
                         [](const EventRecord& a, const EventRecord& b) { return a.ts_ms > b.ts_ms; });
        dropped += ring.size() - opt.backfill_cap;
        ring.erase(ring.begin() + keep, ring.end());
    }

    void finish_backfill(std::vector<EventRecord>& out) {
        if (!backfilling) return;
        backfilling = false;
        backfill_done = true;
        trim_ring();
        std::stable_sort(ring.begin(), ring.end(),
                         [](const EventRecord& a, const EventRecord& b) { return a.ts_ms < b.ts_ms; });
        for (EventRecord& r : ring) out.push_back(std::move(r));
        ring.clear();
        ring.shrink_to_fit();  // backfill happens once; do not keep its buffer
    }

    bool poll(std::vector<EventRecord>& out) {
        if (!sub) {
            if (GetTickCount64() < next_retry) return false;
            if (!subscribe()) {
                schedule_retry();
                return false;
            }
        }
        const std::int64_t cutoff = now_unix_ms() - opt.backfill_ms;
        std::size_t emitted = 0;
        for (;;) {
            DWORD want = kBatch;
            if (backfilling) {
                if (backfill_reads >= opt.max_backfill_reads) {
                    finish_backfill(out);
                    break;
                }
            } else {
                if (emitted >= opt.max_per_poll) break;
                // Pull subscription: only call EvtNext when the signal is set, otherwise it can
                // fail with ERROR_INVALID_OPERATION (Microsoft's pull-subscription sample does the same).
                if (signal && WaitForSingleObject(signal, 0) != WAIT_OBJECT_0) break;
                want = static_cast<DWORD>(std::min<std::size_t>(kBatch, opt.max_per_poll - emitted));
            }
            DWORD returned = 0;
            if (!EvtNext(sub, want, batch, 0, 0, &returned)) {
                const DWORD err = GetLastError();
                if (err == ERROR_NO_MORE_ITEMS || err == ERROR_TIMEOUT || err == ERROR_INVALID_OPERATION) {
                    if (signal) ResetEvent(signal);
                    finish_backfill(out);
                    break;
                }
                log::warn("EvtNext failed: " + log::win32_error(err) + "; resubscribing later");
                finish_backfill(out);
                close_sub();
                schedule_retry();
                return false;
            }
            if (backfilling) backfill_reads += returned;
            for (DWORD i = 0; i < returned; ++i) {
                EventRecord rec;
                const bool ok = to_record(batch[i], rec);
                if (bookmark && EvtUpdateBookmark(bookmark, batch[i])) bookmark_used = true;
                EvtClose(batch[i]);
                batch[i] = nullptr;
                if (!ok) continue;
                if (backfilling) {
                    if (rec.ts_ms < cutoff) continue;  // in case the query could not filter by time
                    ring.push_back(std::move(rec));
                    if (ring.size() >= 2 * opt.backfill_cap) trim_ring();  // amortised bound
                } else {
                    out.push_back(std::move(rec));
                    ++emitted;
                }
            }
            if (backfilling && returned == 0) {
                finish_backfill(out);
                break;
            }
        }
        if (!backfill_done && !backfilling) backfill_done = true;
        return true;
    }
};

EventCollector::EventCollector() : EventCollector(Options{}) {}
EventCollector::EventCollector(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
EventCollector::~EventCollector() = default;

bool EventCollector::poll(std::vector<telemetry::EventRecord>& out) { return impl_->poll(out); }
bool EventCollector::subscribed() const noexcept { return impl_->sub != nullptr; }
bool EventCollector::backfill_done() const noexcept { return impl_->backfill_done; }
std::uint64_t EventCollector::records_dropped() const noexcept { return impl_->dropped; }

}  // namespace opensweep::win
