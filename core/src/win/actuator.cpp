#include "actuator.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "log.hpp"
#include "process_info.hpp"

namespace opensweep::win {

namespace {

namespace abi = procinfo::abi;
using telemetry::ApplyResult;
using telemetry::PolicyKind;
using telemetry::PolicyTarget;

// ---- CPU set APIs (kernel32, Windows 10+), resolved at run time --------------------------------
using GetProcessDefaultCpuSetsFn = BOOL(WINAPI*)(HANDLE, PULONG, ULONG, PULONG);
using SetProcessDefaultCpuSetsFn = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
using GetSystemCpuSetInformationFn = BOOL(WINAPI*)(void*, ULONG, PULONG, HANDLE, ULONG);

struct CpuSetApi {
    GetProcessDefaultCpuSetsFn get_default = nullptr;
    SetProcessDefaultCpuSetsFn set_default = nullptr;
    GetSystemCpuSetInformationFn get_system = nullptr;
    bool available() const { return get_default && set_default && get_system; }
};

template <typename Fn>
Fn resolve(HMODULE m, const char* name) {
    return m ? reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(m, name))) : nullptr;
}

const CpuSetApi& cpu_set_api() {
    static const CpuSetApi api = [] {
        CpuSetApi a;
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        a.get_default = resolve<GetProcessDefaultCpuSetsFn>(k32, "GetProcessDefaultCpuSets");
        a.set_default = resolve<SetProcessDefaultCpuSetsFn>(k32, "SetProcessDefaultCpuSets");
        a.get_system = resolve<GetSystemCpuSetInformationFn>(k32, "GetSystemCpuSetInformation");
        return a;
    }();
    return api;
}

// ---- helpers -----------------------------------------------------------------------------------
ApplyResult failure(std::string error, std::int64_t original = 0) {
    ApplyResult r;
    r.ok = false;
    r.original = original;
    r.error = std::move(error);
    return r;
}

ApplyResult success(std::int64_t original) {
    ApplyResult r;
    r.ok = true;
    r.original = original;
    return r;
}

DWORD rights_for(PolicyKind kind) {
    return kind == PolicyKind::CpuSets ? (PROCESS_QUERY_LIMITED_INFORMATION | abi::kProcessSetLimitedInformation)
                                       : (PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION);
}

bool valid_priority_class(std::int64_t v) {
    switch (v) {
        case IDLE_PRIORITY_CLASS:
        case BELOW_NORMAL_PRIORITY_CLASS:
        case NORMAL_PRIORITY_CLASS:
        case ABOVE_NORMAL_PRIORITY_CLASS:
        case HIGH_PRIORITY_CLASS:
            return true;  // REALTIME is never set by OpenSweep
        default:
            return false;
    }
}

bool get_cpu_sets(HANDLE h, std::vector<ULONG>& ids, DWORD& err) {
    const CpuSetApi& api = cpu_set_api();
    ids.clear();
    if (!api.available()) {
        err = ERROR_CALL_NOT_IMPLEMENTED;
        return false;
    }
    ULONG count = 0;
    if (api.get_default(h, nullptr, 0, &count)) return true;  // no default CPU sets
    err = GetLastError();
    if (err != ERROR_INSUFFICIENT_BUFFER || count == 0 || count > 4096) return false;
    ids.resize(count);
    if (!api.get_default(h, ids.data(), count, &count)) {
        err = GetLastError();
        ids.clear();
        return false;
    }
    ids.resize(count);
    return true;
}

// Reads the current value with handle `h`. On failure returns false and sets `err`.
bool get_value(PolicyKind kind, HANDLE h, std::int64_t& v, DWORD& err) {
    switch (kind) {
        case PolicyKind::EcoQoS: {
            abi::PowerThrottlingState s{};
            s.Version = abi::kPowerThrottlingCurrentVersion;
            if (!GetProcessInformation(h, ProcessPowerThrottling, &s, sizeof(s))) {
                err = GetLastError();
                return false;
            }
            if (s.ControlMask & abi::kPowerThrottlingExecutionSpeed)
                v = (s.StateMask & abi::kPowerThrottlingExecutionSpeed) ? 1 : 0;
            else
                v = -1;
            return true;
        }
        case PolicyKind::MemoryPriority: {
            abi::MemoryPriorityInformation m{};
            if (!GetProcessInformation(h, ProcessMemoryPriority, &m, sizeof(m))) {
                err = GetLastError();
                return false;
            }
            v = static_cast<std::int64_t>(m.MemoryPriority);
            return true;
        }
        case PolicyKind::PriorityClass: {
            const DWORD c = GetPriorityClass(h);
            if (c == 0) {
                err = GetLastError();
                return false;
            }
            v = static_cast<std::int64_t>(c);
            return true;
        }
        case PolicyKind::CpuSets: {
            std::vector<ULONG> ids;
            if (!get_cpu_sets(h, ids, err)) return false;
            v = static_cast<std::int64_t>(ids.size());
            return true;
        }
    }
    err = ERROR_INVALID_PARAMETER;
    return false;
}

bool set_value(PolicyKind kind, HANDLE h, std::int64_t v, const std::vector<ULONG>* cpu_ids, DWORD& err) {
    BOOL ok = FALSE;
    switch (kind) {
        case PolicyKind::EcoQoS: {
            abi::PowerThrottlingState s{};
            s.Version = abi::kPowerThrottlingCurrentVersion;
            if (v >= 0) s.ControlMask = abi::kPowerThrottlingExecutionSpeed;
            if (v == 1) s.StateMask = abi::kPowerThrottlingExecutionSpeed;
            ok = SetProcessInformation(h, ProcessPowerThrottling, &s, sizeof(s));
            break;
        }
        case PolicyKind::MemoryPriority: {
            abi::MemoryPriorityInformation m{};
            m.MemoryPriority = static_cast<ULONG>(v);
            ok = SetProcessInformation(h, ProcessMemoryPriority, &m, sizeof(m));
            break;
        }
        case PolicyKind::PriorityClass:
            ok = SetPriorityClass(h, static_cast<DWORD>(v));
            break;
        case PolicyKind::CpuSets: {
            const CpuSetApi& api = cpu_set_api();
            if (!api.available()) {
                err = ERROR_CALL_NOT_IMPLEMENTED;
                return false;
            }
            if (!cpu_ids || cpu_ids->empty())
                ok = api.set_default(h, nullptr, 0);  // clear: back to the system default
            else
                ok = api.set_default(h, cpu_ids->data(), static_cast<ULONG>(cpu_ids->size()));
            break;
        }
    }
    if (!ok) err = GetLastError();
    return ok != FALSE;
}

std::string kind_name(PolicyKind k) { return telemetry::to_string(k); }

}  // namespace

// ------------------------------------------------------------------------------------------------

WinActuator::WinActuator() : WinActuator(Options{}) {}

WinActuator::WinActuator(Options options) : opt_(options), self_pid_(GetCurrentProcessId()) {}

std::vector<std::uint32_t> WinActuator::efficiency_cpu_set_ids() {
    std::vector<std::uint32_t> out;
    const CpuSetApi& api = cpu_set_api();
    if (!api.available()) return out;
    ULONG len = 0;
    api.get_system(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (len == 0 || len > 16 * 1024 * 1024) return out;
    // SYSTEM_CPU_SET_INFORMATION contains 8-byte members; keep the buffer suitably aligned.
    std::unique_ptr<std::uint64_t[]> buf(new std::uint64_t[(len + 7) / 8]);
    if (!api.get_system(buf.get(), len, &len, GetCurrentProcess(), 0))
        return out;
    struct Entry {
        std::uint32_t id;
        BYTE cls;
    };
    std::vector<Entry> entries;
    BYTE min_cls = 0xFF, max_cls = 0;
    const auto* base = reinterpret_cast<const BYTE*>(buf.get());
    ULONG off = 0;
    while (off + sizeof(DWORD) * 2 <= len) {
        DWORD size = 0, type = 0;
        std::memcpy(&size, base + off, sizeof(size));
        std::memcpy(&type, base + off + sizeof(DWORD), sizeof(type));
        if (size == 0 || off + size > len) break;
        if (type == 0 /* CpuSetInformation */ && size >= sizeof(abi::CpuSetEntry)) {
            abi::CpuSetEntry e{};
            std::memcpy(&e, base + off, sizeof(e));
            const BYTE cls = e.EfficiencyClass;
            min_cls = std::min(min_cls, cls);
            max_cls = std::max(max_cls, cls);
            // Skip CPU sets reserved for exclusive/real-time use by other software.
            if (!(e.AllFlags & (abi::kCpuSetAllocated | abi::kCpuSetRealTime)))
                entries.push_back({static_cast<std::uint32_t>(e.Id), cls});
        }
        off += size;
    }
    if (entries.empty() || min_cls == max_cls) return out;  // not hybrid
    for (const Entry& e : entries)
        if (e.cls == min_cls) out.push_back(e.id);
    return out;
}

namespace {

// Opens `target` with `access` and applies the guards described in actuator.hpp.
// `strict` = apply-time checks (name lists, protection, criticality, session).
bool open_checked(const PolicyTarget& t, DWORD access, bool strict, bool allow_session0, std::uint32_t self_pid,
                  procinfo::ProcessHandle& out, std::string& err) {
    if (t.pid == 0 || t.pid == 4) {
        err = "refused: system process (pid " + std::to_string(t.pid) + ")";
        return false;
    }
    if (t.pid == self_pid) {
        err = "refused: OpenSweep's own process";
        return false;
    }
    if (t.process_start_ms == 0) {
        err = "refused: unknown process start time (PID reuse guard)";
        return false;
    }
    if (strict && !t.name.empty() &&
        (procinfo::is_protected_name(t.name) || procinfo::is_security_software_name(t.name))) {
        err = "refused: '" + t.name + "' is on the protection list";
        return false;
    }
    out = procinfo::open_process(t.pid, access);
    if (!out) {
        const DWORD e = GetLastError();
        err = e == ERROR_INVALID_PARAMETER ? "process no longer exists" : "cannot open process: " + log::win32_error(e);
        return false;
    }
    if (!procinfo::is_running(out.get())) {
        err = "process no longer exists";
        return false;
    }
    std::int64_t start = 0;
    if (!procinfo::creation_time_ms(out.get(), start)) {
        err = "cannot read process start time: " + log::win32_error(GetLastError());
        return false;
    }
    if (start != t.process_start_ms) {
        err = "refused: process start time mismatch (PID reused)";
        return false;
    }
    if (!strict) return true;

    std::wstring path;
    if (!procinfo::image_path(out.get(), path)) {
        err = "refused: cannot read the image name";
        return false;
    }
    const std::string actual = log::narrow(procinfo::file_name_of(path));
    if (procinfo::is_protected_name(actual) || procinfo::is_security_software_name(actual)) {
        err = "refused: '" + actual + "' is on the protection list";
        return false;
    }
    if (!t.name.empty() && procinfo::normalize_name(t.name) != procinfo::normalize_name(actual)) {
        err = "refused: image name mismatch ('" + t.name + "' vs '" + actual + "')";
        return false;
    }
    unsigned long level = procinfo::kProtectionLevelNone;
    if (procinfo::protection_level(out.get(), level) && level != procinfo::kProtectionLevelNone) {
        err = "refused: protected process";
        return false;
    }
    bool critical = false;
    if (procinfo::is_critical(out.get(), critical) && critical) {
        err = "refused: critical process";
        return false;
    }
    std::uint32_t session = 0;
    if (!procinfo::session_of(t.pid, session)) {
        err = "refused: cannot determine the session";
        return false;
    }
    if (session == 0 && !allow_session0) {
        err = "refused: session 0 (service) process";
        return false;
    }
    return true;
}

// get_value, retrying once with PROCESS_QUERY_INFORMATION if the limited right is not enough.
bool get_value_robust(PolicyKind kind, HANDLE h, const PolicyTarget& t, std::int64_t& v, DWORD& err) {
    if (get_value(kind, h, v, err)) return true;
    if (err != ERROR_ACCESS_DENIED) return false;
    procinfo::ProcessHandle full = procinfo::open_process(t.pid, PROCESS_QUERY_INFORMATION);
    std::int64_t start = 0;
    if (!full || !procinfo::creation_time_ms(full.get(), start) || start != t.process_start_ms) return false;
    return get_value(kind, full.get(), v, err);
}

}  // namespace

bool WinActuator::read(PolicyKind kind, const PolicyTarget& target, std::int64_t& value, std::string* error) {
    procinfo::ProcessHandle h;
    std::string err;
    if (!open_checked(target, PROCESS_QUERY_LIMITED_INFORMATION, false, true, self_pid_, h, err)) {
        if (error) *error = err;
        return false;
    }
    DWORD e = 0;
    if (!get_value_robust(kind, h.get(), target, value, e)) {
        if (error) *error = "cannot read " + kind_name(kind) + ": " + log::win32_error(e);
        return false;
    }
    return true;
}

ApplyResult WinActuator::apply(PolicyKind kind, const PolicyTarget& target, std::int64_t value) {
    // Value validation first (cheap, no handle needed).
    switch (kind) {
        case PolicyKind::EcoQoS:
            if (value != 1) return failure("refused: EcoQoS apply only supports value 1 (on)");
            break;
        case PolicyKind::MemoryPriority:
            if (value < abi::kMemoryPriorityLow || value > abi::kMemoryPriorityBelowNormal)
                return failure("refused: memory priority apply must be 2..4");
            break;
        case PolicyKind::PriorityClass:
            if (value != BELOW_NORMAL_PRIORITY_CLASS) return failure("refused: only BELOW_NORMAL_PRIORITY_CLASS is allowed");
            break;
        case PolicyKind::CpuSets:
            if (value < 1) return failure("refused: CPU set count must be >= 1");
            break;
    }

    std::lock_guard<std::mutex> lock(mu_);
    procinfo::ProcessHandle h;
    std::string err;
    if (!open_checked(target, rights_for(kind), true, opt_.allow_session0, self_pid_, h, err)) return failure(err);

    DWORD e = 0;
    std::int64_t original = 0;
    if (!get_value_robust(kind, h.get(), target, original, e)) {
        if (kind == PolicyKind::EcoQoS && (e == ERROR_INVALID_PARAMETER || e == ERROR_NOT_SUPPORTED)) {
            // Older builds cannot query ProcessPowerThrottling; an untouched process is at the
            // system default, which is what restore(-1) re-establishes.
            original = -1;
            log::debug("ProcessPowerThrottling query unsupported; assuming system default as original");
        } else {
            return failure("cannot read current " + kind_name(kind) + ": " + log::win32_error(e));
        }
    }

    std::vector<ULONG> new_ids;
    std::vector<ULONG> old_ids;
    switch (kind) {
        case PolicyKind::EcoQoS:
            if (original == 1) return failure("no change: EcoQoS already on", original);
            break;
        case PolicyKind::MemoryPriority:
            if (value >= original) return failure("no change: memory priority is not lowered", original);
            break;
        case PolicyKind::PriorityClass:
            if (original != NORMAL_PRIORITY_CLASS)
                return failure("refused: priority class is not NORMAL (was changed by someone else)", original);
            break;
        case PolicyKind::CpuSets: {
            if (original != 0) return failure("refused: process already has its own default CPU sets", original);
            const std::vector<std::uint32_t> eff = efficiency_cpu_set_ids();
            if (eff.empty()) return failure("refused: not a hybrid CPU (or CPU set API unavailable)", original);
            const std::size_t n = std::min<std::size_t>(eff.size(), static_cast<std::size_t>(value));
            new_ids.assign(eff.begin(), eff.begin() + static_cast<std::ptrdiff_t>(n));
            DWORD ge = 0;
            if (!get_cpu_sets(h.get(), old_ids, ge)) return failure("cannot read CPU sets: " + log::win32_error(ge), original);
            break;
        }
    }

    if (!set_value(kind, h.get(), value, &new_ids, e))
        return failure("set " + kind_name(kind) + " failed: " + log::win32_error(e), original);

    // Verify (where readable); undo on mismatch.
    std::int64_t now = 0;
    DWORD ve = 0;
    const std::int64_t expected = kind == PolicyKind::CpuSets ? static_cast<std::int64_t>(new_ids.size()) : value;
    if (get_value_robust(kind, h.get(), target, now, ve) && now != expected) {
        DWORD ue = 0;
        set_value(kind, h.get(), original, &old_ids, ue);
        return failure("verification failed for " + kind_name(kind) + " (read back " + std::to_string(now) + ")",
                       original);
    }

    if (kind == PolicyKind::CpuSets) {
        std::vector<std::uint32_t> saved(old_ids.begin(), old_ids.end());
        saved_cpu_sets_[{target.pid, target.process_start_ms}] = std::move(saved);
    }
    log::info("actuator: applied " + kind_name(kind) + "=" + std::to_string(value) + " to pid " +
              std::to_string(target.pid) + " (original " + std::to_string(original) + ")");
    return success(original);
}

ApplyResult WinActuator::restore(PolicyKind kind, const PolicyTarget& target, std::int64_t original) {
    switch (kind) {
        case PolicyKind::EcoQoS:
            if (original < -1 || original > 1) return failure("invalid EcoQoS original value");
            break;
        case PolicyKind::MemoryPriority:
            if (original < abi::kMemoryPriorityVeryLow || original > abi::kMemoryPriorityNormal)
                return failure("invalid memory priority original value");
            break;
        case PolicyKind::PriorityClass:
            if (!valid_priority_class(original)) return failure("invalid priority class original value");
            break;
        case PolicyKind::CpuSets:
            if (original < 0) return failure("invalid CPU set original value");
            break;
    }

    std::lock_guard<std::mutex> lock(mu_);
    const auto key = std::make_pair(target.pid, target.process_start_ms);
    std::vector<ULONG> ids;
    bool have_saved = false;
    if (kind == PolicyKind::CpuSets) {
        auto it = saved_cpu_sets_.find(key);
        if (it != saved_cpu_sets_.end()) {
            ids.assign(it->second.begin(), it->second.end());
            have_saved = true;
            saved_cpu_sets_.erase(it);  // the process is restored or gone either way
        }
    }

    procinfo::ProcessHandle h;
    std::string err;
    if (!open_checked(target, rights_for(kind), false, true, self_pid_, h, err)) return failure(err);

    DWORD e = 0;
    std::int64_t before = 0;
    if (!get_value_robust(kind, h.get(), target, before, e)) before = 0;  // informational only

    if (kind == PolicyKind::CpuSets && !have_saved && original != 0)
        log::warn("actuator: CPU set id list for pid " + std::to_string(target.pid) +
                  " unknown (restart?); restoring the system default instead");

    if (!set_value(kind, h.get(), original, &ids, e))
        return failure("restore " + kind_name(kind) + " failed: " + log::win32_error(e), before);
    log::info("actuator: restored " + kind_name(kind) + "=" + std::to_string(original) + " on pid " +
              std::to_string(target.pid));
    return success(before);
}

bool WinActuator::alive(const PolicyTarget& target) {
    if (target.pid == 0) return false;
    procinfo::ProcessHandle h = procinfo::open_process(target.pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (!h || !procinfo::is_running(h.get())) return false;
    if (target.process_start_ms == 0) return true;
    std::int64_t start = 0;
    return procinfo::creation_time_ms(h.get(), start) && start == target.process_start_ms;
}

}  // namespace opensweep::win
