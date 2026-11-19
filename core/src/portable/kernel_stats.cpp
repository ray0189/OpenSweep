#include "kernel_stats.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace opensweep::kstats {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string_view file_part(std::string_view path) {
    const auto p = path.find_last_of("\\/");
    return p == std::string_view::npos ? path : path.substr(p + 1);
}

std::int64_t align_minute(std::int64_t t) {
    std::int64_t q = t / kWindowMs;
    if (t % kWindowMs != 0 && t < 0) --q;
    return q * kWindowMs;
}

double round_to(double v, double step) { return std::isfinite(v) ? std::round(v / step) * step : 0.0; }

struct Known {
    std::string_view name;
    std::string_view description;
};

// Well-known Windows / vendor kernel images and their plain-language class.
constexpr Known kKnown[] = {
    {"ntoskrnl.exe", "Windows kernel"},
    {"hal.dll", "hardware abstraction layer"},
    {"ndis.sys", "network driver"},
    {"tcpip.sys", "network stack"},
    {"netio.sys", "network stack"},
    {"afd.sys", "network sockets"},
    {"wfplwfs.sys", "network filtering"},
    {"netwtw04.sys", "Wi-Fi driver (Intel)"},
    {"netwtw06.sys", "Wi-Fi driver (Intel)"},
    {"netwtw08.sys", "Wi-Fi driver (Intel)"},
    {"netwtw10.sys", "Wi-Fi driver (Intel)"},
    {"netwtw12.sys", "Wi-Fi driver (Intel)"},
    {"rt640x64.sys", "network driver (Realtek)"},
    {"rtux64w10.sys", "USB network driver (Realtek)"},
    {"e1d68x64.sys", "network driver (Intel)"},
    {"e1i68x64.sys", "network driver (Intel)"},
    {"bcmwl63a.sys", "Wi-Fi driver (Broadcom)"},
    {"athw8x.sys", "Wi-Fi driver (Qualcomm Atheros)"},
    {"nvlddmkm.sys", "graphics driver (NVIDIA)"},
    {"amdkmdag.sys", "graphics driver (AMD)"},
    {"atikmdag.sys", "graphics driver (AMD)"},
    {"igdkmd64.sys", "graphics driver (Intel)"},
    {"igdkmdn64.sys", "graphics driver (Intel)"},
    {"igdkmdnd64.sys", "graphics driver (Intel)"},
    {"dxgkrnl.sys", "graphics kernel (DirectX)"},
    {"dxgmms2.sys", "graphics memory manager (DirectX)"},
    {"storport.sys", "storage driver"},
    {"stornvme.sys", "storage driver (NVMe)"},
    {"storahci.sys", "storage driver (SATA AHCI)"},
    {"iastora.sys", "storage driver (Intel RST)"},
    {"iastorac.sys", "storage driver (Intel RST)"},
    {"iastorvd.sys", "storage driver (Intel RST)"},
    {"ataport.sys", "storage driver (ATA)"},
    {"disk.sys", "disk class driver"},
    {"classpnp.sys", "storage class driver"},
    {"ntfs.sys", "file system (NTFS)"},
    {"fltmgr.sys", "file system filter manager"},
    {"usbxhci.sys", "USB controller driver"},
    {"usbport.sys", "USB controller driver"},
    {"usbhub3.sys", "USB hub driver"},
    {"ucx01000.sys", "USB controller extension"},
    {"hdaudbus.sys", "audio bus driver"},
    {"hdaudio.sys", "audio driver"},
    {"portcls.sys", "audio port driver"},
    {"ks.sys", "kernel streaming (audio/video)"},
    {"rtkvhd64.sys", "audio driver (Realtek)"},
    {"intcaudiobus.sys", "audio bus driver (Intel)"},
    {"acpi.sys", "power management (ACPI)"},
    {"intelppm.sys", "processor power management (Intel)"},
    {"amdppm.sys", "processor power management (AMD)"},
    {"processr.sys", "processor power management"},
    {"pci.sys", "PCI bus driver"},
    {"wdf01000.sys", "driver framework (KMDF)"},
    {"i8042prt.sys", "keyboard/mouse (PS/2) driver"},
    {"kbdclass.sys", "keyboard driver"},
    {"mouclass.sys", "mouse driver"},
    {"hidclass.sys", "input device (HID) driver"},
    {"hidusb.sys", "USB input device driver"},
    {"bthport.sys", "Bluetooth driver"},
    {"bthusb.sys", "Bluetooth driver"},
    {"wdfilter.sys", "security software (Microsoft Defender)"},
    {"wd.sys", "security software (Microsoft Defender)"},
    {"vmbus.sys", "virtual machine bus (Hyper-V)"},
    {"storvsc.sys", "virtual storage (Hyper-V)"},
    {"netvsc.sys", "virtual network (Hyper-V)"},
    {"tpm.sys", "TPM driver"},
};

template <typename T, typename Key>
void top_n(std::vector<T>& v, std::size_t n, Key key, std::uint64_t& dropped) {
    std::sort(v.begin(), v.end(), [&](const T& a, const T& b) { return key(a) > key(b); });
    if (v.size() > n) {
        dropped += v.size() - n;
        v.resize(n);
    }
}

}  // namespace

std::size_t histogram_bucket(double us) noexcept {
    for (std::size_t i = 0; i < kBucketUpperUs.size(); ++i)
        if (us < kBucketUpperUs[i]) return i;
    return kHistogramBuckets - 1;
}

std::uint32_t flags_for_level(int level) noexcept {
    const std::uint32_t base = flag::kProcess | flag::kThread | flag::kImageLoad;
    switch (level) {
        case 0: return base | flag::kDpc | flag::kInterrupt | flag::kDiskIo | flag::kHardFaults;
        case 1: return base | flag::kDiskIo | flag::kHardFaults;
        case 2: return base | flag::kHardFaults;
        default: return 0;
    }
}

std::vector<std::string> flag_names(std::uint32_t flags) {
    static constexpr std::pair<std::uint32_t, const char*> kNames[] = {
        {flag::kProcess, "PROCESS"}, {flag::kThread, "THREAD"},       {flag::kImageLoad, "IMAGE_LOAD"},
        {flag::kDpc, "DPC"},         {flag::kInterrupt, "INTERRUPT"}, {flag::kDiskIo, "DISK_IO"},
        {flag::kHardFaults, "HARD_FAULTS"},
    };
    std::vector<std::string> out;
    for (const auto& [f, n] : kNames)
        if (flags & f) out.emplace_back(n);
    return out;
}

std::string driver_description(std::string_view file_name) {
    const std::string n = lower(file_part(file_name));
    for (const auto& k : kKnown)
        if (n == k.name) return std::string(k.description);
    return {};
}

void DriverStats::merge(const DriverStats& o) {
    dpc_count += o.dpc_count;
    isr_count += o.isr_count;
    dpc_us += o.dpc_us;
    isr_us += o.isr_us;
    max_us = std::max(max_us, o.max_us);
    for (std::size_t i = 0; i < kHistogramBuckets; ++i) histogram[i] += o.histogram[i];
}

void ProcessDisk::merge(const ProcessDisk& o) {
    read_bytes += o.read_bytes;
    write_bytes += o.write_bytes;
    ios += o.ios;
    latency_sum_ms += o.latency_sum_ms;
    max_latency_ms = std::max(max_latency_ms, o.max_latency_ms);
}

void KernelStats::Acc::clear() {
    start_ms = -1;
    last_ms = -1;
    dpc_us = isr_us = worst_dpc_us = worst_isr_us = 0.0;
    dpc_count = isr_count = hard_faults = disk_bytes = dropped = 0;
    drivers.clear();
    disk.clear();
    faults.clear();
}

KernelStats::KernelStats(Config cfg) : cfg_(cfg) {
    if (cfg_.logical_processors == 0) cfg_.logical_processors = 1;
    if (cfg_.windows == 0) cfg_.windows = 1;
    if (cfg_.top_keys == 0) cfg_.top_keys = 1;
    names_.emplace_back(kUnknownName);
}

// ---- maps -------------------------------------------------------------------------------------
std::uint32_t KernelStats::driver_id(std::string_view file_name) {
    const std::string_view f = file_part(file_name);
    if (f.empty()) return 0;
    std::string key = lower(f);
    if (key == kUnknownName) return 0;
    if (auto it = name_ids_.find(key); it != name_ids_.end()) return it->second;
    if (names_.size() >= kMaxDriverNames) return 0;
    const auto id = static_cast<std::uint32_t>(names_.size());
    names_.emplace_back(f);
    name_ids_.emplace(std::move(key), id);
    return id;
}

void KernelStats::on_image_load(std::uint64_t base, std::uint64_t size, std::string_view file_name) {
    ++counters_.events;
    const std::uint64_t end = base + size;
    if (size == 0 || end < base || size > (1ull << 32)) {
        ++counters_.malformed;
        return;
    }
    // Remove ranges overlapping the new one (an unload we missed, or a rundown repeat).
    std::erase_if(images_, [&](const Image& im) { return im.base < end && base < im.end; });
    if (images_.size() >= kMaxImages) {
        ++counters_.map_resets;
        return;
    }
    const Image im{base, end, driver_id(file_name)};
    images_.insert(std::upper_bound(images_.begin(), images_.end(), base,
                                    [](std::uint64_t b, const Image& x) { return b < x.base; }),
                   im);
}

void KernelStats::on_image_unload(std::uint64_t base) {
    ++counters_.events;
    std::erase_if(images_, [&](const Image& im) { return im.base == base; });
}

std::uint32_t KernelStats::lookup(std::uint64_t address) const {
    auto it = std::upper_bound(images_.begin(), images_.end(), address,
                               [](std::uint64_t a, const Image& x) { return a < x.base; });
    if (it == images_.begin()) return 0;
    --it;
    return address < it->end ? it->id : 0;
}

std::string KernelStats::driver_for(std::uint64_t address) const { return names_[lookup(address)]; }

void KernelStats::on_thread_start(std::uint32_t tid, std::uint32_t pid) {
    ++counters_.events;
    if (threads_.size() >= kMaxThreads && threads_.find(tid) == threads_.end()) {
        threads_.clear();
        ++counters_.map_resets;
    }
    threads_[tid] = pid;
}

void KernelStats::on_thread_end(std::uint32_t tid) {
    ++counters_.events;
    threads_.erase(tid);
}

std::uint32_t KernelStats::pid_for_thread(std::uint32_t tid) const {
    const auto it = threads_.find(tid);
    return it == threads_.end() ? kOtherPid : it->second;
}

void KernelStats::on_process_start(std::uint32_t pid, std::string_view image_name) {
    ++counters_.events;
    if (procs_.size() >= kMaxProcesses && procs_.find(pid) == procs_.end()) {
        procs_.clear();
        ++counters_.map_resets;
    }
    std::string name(file_part(image_name));
    // The kernel's image name is limited to 15 characters; prefer the full name when available.
    if (name.size() >= 14 && resolver_) {
        std::string full = resolver_(pid);
        if (!full.empty()) name = std::move(full);
    }
    procs_[pid] = std::move(name);
}

void KernelStats::on_process_end(std::uint32_t pid) {
    ++counters_.events;
    procs_.erase(pid);
}

std::string KernelStats::process_name(std::uint32_t pid) {
    if (pid == 0) return kUnknownName;
    if (pid == 4) return "System";
    if (pid == kOtherPid) return kOtherName;
    if (auto it = procs_.find(pid); it != procs_.end()) return it->second.empty() ? kUnknownName : it->second;
    std::string name;
    if (resolver_) name = resolver_(pid);
    if (procs_.size() < kMaxProcesses) procs_[pid] = name;  // also caches a failed lookup
    return name.empty() ? kUnknownName : name;
}

void KernelStats::reset_maps() {
    images_.clear();
    threads_.clear();
    procs_.clear();
}

// ---- windows ----------------------------------------------------------------------------------
void KernelStats::advance(std::int64_t now) {
    if (cur_.start_ms < 0) {
        cur_.start_ms = align_minute(now);
        return;
    }
    if (now >= cur_.start_ms + kWindowMs || now < cur_.start_ms - kWindowMs) {
        close_window();
        cur_.start_ms = align_minute(now);
    }
}

Window KernelStats::to_window(const Acc& a, std::int64_t end_ms) const {
    Window w;
    w.start_ms = a.start_ms;
    w.end_ms = end_ms;
    w.dpc_us = a.dpc_us;
    w.isr_us = a.isr_us;
    w.worst_dpc_us = a.worst_dpc_us;
    w.worst_isr_us = a.worst_isr_us;
    w.dpc_count = a.dpc_count;
    w.isr_count = a.isr_count;
    w.hard_faults = a.hard_faults;
    w.disk_bytes = a.disk_bytes;
    w.keys_dropped = a.dropped;
    w.drivers.reserve(a.drivers.size());
    for (const auto& [id, d] : a.drivers) w.drivers.push_back(d);
    w.disk.reserve(a.disk.size());
    for (const auto& [pid, d] : a.disk) w.disk.push_back(d);
    w.faults.reserve(a.faults.size());
    for (const auto& [pid, f] : a.faults) w.faults.push_back(f);
    top_n(w.drivers, cfg_.top_keys, [](const DriverStats& d) { return d.total_us(); }, w.keys_dropped);
    top_n(w.disk, cfg_.top_keys, [](const ProcessDisk& d) { return d.read_bytes + d.write_bytes; }, w.keys_dropped);
    top_n(w.faults, cfg_.top_keys, [](const ProcessFaults& f) { return f.faults; }, w.keys_dropped);
    return w;
}

void KernelStats::close_window() {
    if (cur_.start_ms >= 0 && (cur_.dpc_count || cur_.isr_count || cur_.hard_faults || !cur_.disk.empty())) {
        windows_.push_back(to_window(cur_, cur_.start_ms + kWindowMs));
        while (windows_.size() > cfg_.windows) windows_.pop_front();
    }
    cur_.clear();
}

Window KernelStats::merged(std::int64_t now, std::int64_t span_ms, std::int64_t& covered_ms) {
    advance(now);
    const std::int64_t from = now - span_ms;
    Window out;
    std::int64_t earliest = now;
    std::map<std::string, DriverStats> drivers;
    std::map<std::pair<std::uint32_t, std::string>, ProcessDisk> disk;
    std::map<std::pair<std::uint32_t, std::string>, ProcessFaults> faults;
    auto add = [&](const Window& w) {
        earliest = std::min(earliest, w.start_ms);
        out.dpc_us += w.dpc_us;
        out.isr_us += w.isr_us;
        out.worst_dpc_us = std::max(out.worst_dpc_us, w.worst_dpc_us);
        out.worst_isr_us = std::max(out.worst_isr_us, w.worst_isr_us);
        out.dpc_count += w.dpc_count;
        out.isr_count += w.isr_count;
        out.hard_faults += w.hard_faults;
        out.disk_bytes += w.disk_bytes;
        out.keys_dropped += w.keys_dropped;
        for (const auto& d : w.drivers) {
            auto& m = drivers[d.name];
            if (m.name.empty()) m.name = d.name;
            m.merge(d);
        }
        for (const auto& d : w.disk) {
            auto& m = disk[{d.pid, d.name}];
            if (m.name.empty()) {
                m.pid = d.pid;
                m.name = d.name;
            }
            m.merge(d);
        }
        for (const auto& f : w.faults) {
            auto& m = faults[{f.pid, f.name}];
            if (m.name.empty()) {
                m.pid = f.pid;
                m.name = f.name;
            }
            m.faults += f.faults;
        }
    };
    for (const auto& w : windows_)
        if (w.end_ms > from && w.start_ms <= now) add(w);
    if (cur_.start_ms >= 0) add(to_window(cur_, now));
    for (auto& [k, d] : drivers) out.drivers.push_back(std::move(d));
    for (auto& [k, d] : disk) out.disk.push_back(std::move(d));
    for (auto& [k, f] : faults) out.faults.push_back(std::move(f));
    const std::int64_t start = std::max(earliest, first_ms_ >= 0 ? first_ms_ : earliest);
    covered_ms = std::max<std::int64_t>(0, now - start);
    out.start_ms = start;
    out.end_ms = now;
    return out;
}

// ---- events -----------------------------------------------------------------------------------
void KernelStats::add_dpc_isr(Acc& a, std::uint32_t id, Routine kind, double us) {
    DriverStats& d = a.drivers[id];
    if (d.name.empty()) d.name = names_[id];
    const std::size_t b = histogram_bucket(us);
    ++d.histogram[b];
    d.max_us = std::max(d.max_us, us);
    if (kind == Routine::Dpc) {
        ++d.dpc_count;
        d.dpc_us += us;
        ++a.dpc_count;
        a.dpc_us += us;
        a.worst_dpc_us = std::max(a.worst_dpc_us, us);
    } else {
        ++d.isr_count;
        d.isr_us += us;
        ++a.isr_count;
        a.isr_us += us;
        a.worst_isr_us = std::max(a.worst_isr_us, us);
    }
}

void KernelStats::on_dpc_isr(Routine kind, std::uint64_t routine, double duration_us, std::int64_t ts_ms) {
    ++counters_.events;
    if (!std::isfinite(duration_us) || duration_us < 0.0 || duration_us > kMaxPlausibleUs) {
        ++counters_.malformed;
        return;
    }
    ++counters_.dpc_isr;
    note_time(ts_ms);
    const std::uint32_t id = lookup(routine);
    if (id == 0) ++counters_.unknown_routine;
    add_dpc_isr(cur_, id, kind, duration_us);
    add_dpc_isr(tick_, id, kind, duration_us);
}

std::uint32_t KernelStats::attribute(std::uint32_t tid, std::uint32_t fallback_pid) {
    if (auto it = threads_.find(tid); tid != 0 && it != threads_.end()) return it->second;
    if (fallback_pid != kOtherPid && fallback_pid != 0) return fallback_pid;
    ++counters_.unattributed;
    return 0;
}

void KernelStats::add_disk(Acc& a, std::uint32_t pid, std::uint64_t bytes, bool write, double latency_ms) {
    auto it = a.disk.find(pid);
    if (it == a.disk.end()) {
        if (a.disk.size() >= kMaxOpenKeys) {
            ++a.dropped;
            pid = kOtherPid;
            it = a.disk.find(pid);
        }
        if (it == a.disk.end()) {
            ProcessDisk d;
            d.pid = pid;
            d.name = process_name(pid);
            it = a.disk.emplace(pid, std::move(d)).first;
        }
    }
    ProcessDisk& d = it->second;
    (write ? d.write_bytes : d.read_bytes) += bytes;
    ++d.ios;
    d.latency_sum_ms += latency_ms;
    d.max_latency_ms = std::max(d.max_latency_ms, latency_ms);
    a.disk_bytes += bytes;
}

void KernelStats::on_disk_io(std::uint32_t tid, std::uint32_t fallback_pid, std::uint64_t bytes, bool write,
                             double latency_ms, std::int64_t ts_ms) {
    ++counters_.events;
    if (!std::isfinite(latency_ms) || latency_ms < 0.0 || latency_ms > kMaxPlausibleLatencyMs ||
        bytes > (1ull << 32)) {
        ++counters_.malformed;
        return;
    }
    ++counters_.disk;
    note_time(ts_ms);
    const std::uint32_t pid = attribute(tid, fallback_pid);
    add_disk(cur_, pid, bytes, write, latency_ms);
    add_disk(tick_, pid, bytes, write, latency_ms);
}

void KernelStats::add_fault(Acc& a, std::uint32_t pid) {
    auto it = a.faults.find(pid);
    if (it == a.faults.end()) {
        if (a.faults.size() >= kMaxOpenKeys) {
            ++a.dropped;
            pid = kOtherPid;
            it = a.faults.find(pid);
        }
        if (it == a.faults.end()) {
            ProcessFaults f;
            f.pid = pid;
            f.name = process_name(pid);
            it = a.faults.emplace(pid, std::move(f)).first;
        }
    }
    ++it->second.faults;
    ++a.hard_faults;
}

void KernelStats::on_hard_fault(std::uint32_t tid, std::uint32_t fallback_pid, std::int64_t ts_ms) {
    ++counters_.events;
    ++counters_.hard_faults;
    note_time(ts_ms);
    const std::uint32_t pid = attribute(tid, fallback_pid);
    add_fault(cur_, pid);
    add_fault(tick_, pid);
}

void KernelStats::note_time(std::int64_t ts_ms) {
    if (first_ms_ < 0) first_ms_ = ts_ms;
    advance(ts_ms);
    if (tick_.start_ms < 0) tick_.start_ms = ts_ms;
}

// ---- queries ----------------------------------------------------------------------------------
LatencyView KernelStats::latency(std::int64_t now, std::int64_t span_ms, std::size_t top) {
    std::int64_t covered = 0;
    Window w = merged(now, span_ms, covered);
    LatencyView v;
    v.covered_ms = covered;
    const double denom = static_cast<double>(covered) * 1000.0 * cfg_.logical_processors;
    if (denom > 0.0) {
        v.dpc_percent = std::min(100.0, w.dpc_us / denom * 100.0);
        v.isr_percent = std::min(100.0, w.isr_us / denom * 100.0);
    }
    v.worst_dpc_us = w.worst_dpc_us;
    v.worst_isr_us = w.worst_isr_us;
    std::sort(w.drivers.begin(), w.drivers.end(), [](const DriverStats& a, const DriverStats& b) {
        return a.max_us != b.max_us ? a.max_us > b.max_us : a.total_us() > b.total_us();
    });
    if (w.drivers.size() > top) w.drivers.resize(top);
    v.drivers = std::move(w.drivers);
    return v;
}

std::vector<DiskProcessView> KernelStats::disk_activity(std::int64_t now, std::int64_t span_ms, std::size_t top) {
    std::int64_t covered = 0;
    Window w = merged(now, span_ms, covered);
    const double secs = std::max(1.0, static_cast<double>(covered) / 1000.0);
    std::vector<DiskProcessView> out;
    for (const auto& d : w.disk) {
        if (d.pid == kOtherPid) continue;
        DiskProcessView x;
        x.pid = d.pid;
        x.name = d.name;
        x.read_bps = static_cast<double>(d.read_bytes) / secs;
        x.write_bps = static_cast<double>(d.write_bytes) / secs;
        x.ios = d.ios;
        x.mean_latency_ms = d.ios ? d.latency_sum_ms / static_cast<double>(d.ios) : 0.0;
        x.max_latency_ms = d.max_latency_ms;
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const DiskProcessView& a, const DiskProcessView& b) {
        return a.read_bps + a.write_bps > b.read_bps + b.write_bps;
    });
    if (out.size() > top) out.resize(top);
    return out;
}

std::vector<FaultProcessView> KernelStats::hard_faults(std::int64_t now, std::int64_t span_ms, std::size_t top) {
    std::int64_t covered = 0;
    Window w = merged(now, span_ms, covered);
    const double secs = std::max(1.0, static_cast<double>(covered) / 1000.0);
    std::vector<FaultProcessView> out;
    for (const auto& f : w.faults) {
        if (f.pid == kOtherPid) continue;
        out.push_back({f.pid, f.name, static_cast<double>(f.faults) / secs});
    }
    std::sort(out.begin(), out.end(),
              [](const FaultProcessView& a, const FaultProcessView& b) { return a.faults_per_sec > b.faults_per_sec; });
    if (out.size() > top) out.resize(top);
    return out;
}

void KernelStats::fill_sample(std::int64_t now, telemetry::Sample& s, bool running, double cost_percent) {
    advance(now);
    const std::int64_t start = tick_.start_ms >= 0 ? tick_.start_ms : now - telemetry::kSampleIntervalMs;
    const double secs = std::max(0.001, static_cast<double>(now - start) / 1000.0);
    s.kernel_valid = running;
    s.kernel = {};
    s.kernel.running = running;
    s.kernel.cost_percent = cost_percent;
    s.kernel.events_lost = events_lost_;
    const double denom = secs * 1e6 * cfg_.logical_processors;
    s.kernel.dpc_percent = std::min(100.0, tick_.dpc_us / denom * 100.0);
    s.kernel.isr_percent = std::min(100.0, tick_.isr_us / denom * 100.0);
    s.kernel.worst_dpc_us = tick_.worst_dpc_us;
    s.kernel.worst_isr_us = tick_.worst_isr_us;
    s.kernel.hard_faults_per_sec = static_cast<double>(tick_.hard_faults) / secs;
    s.kernel.disk_bytes_per_sec = static_cast<double>(tick_.disk_bytes) / secs;

    // Drivers: the largest total time first; then the longest single routines among the rest.
    std::vector<const DriverStats*> ds;
    ds.reserve(tick_.drivers.size());
    for (const auto& [id, d] : tick_.drivers) ds.push_back(&d);
    std::sort(ds.begin(), ds.end(), [](const DriverStats* a, const DriverStats* b) { return a->total_us() > b->total_us(); });
    const std::size_t by_total = std::min<std::size_t>(ds.size(), telemetry::kMaxKernelDrivers * 2 / 3);
    std::sort(ds.begin() + static_cast<std::ptrdiff_t>(by_total), ds.end(),
              [](const DriverStats* a, const DriverStats* b) { return a->max_us > b->max_us; });
    s.kernel_drivers.clear();
    for (std::size_t i = 0; i < ds.size() && i < telemetry::kMaxKernelDrivers; ++i)
        s.kernel_drivers.push_back({ds[i]->name, ds[i]->max_us, ds[i]->total_us(), ds[i]->count()});

    // Processes: the busiest by disk bytes and by hard faults (union, bounded).
    std::map<std::uint32_t, telemetry::KernelProcessSample> procs;
    std::vector<const ProcessDisk*> dk;
    for (const auto& [pid, d] : tick_.disk)
        if (pid != kOtherPid) dk.push_back(&d);
    std::sort(dk.begin(), dk.end(), [](const ProcessDisk* a, const ProcessDisk* b) {
        return a->read_bytes + a->write_bytes > b->read_bytes + b->write_bytes;
    });
    const std::size_t half = telemetry::kMaxKernelProcesses / 2;
    for (std::size_t i = 0; i < dk.size() && i < half; ++i) {
        auto& p = procs[dk[i]->pid];
        p.pid = dk[i]->pid;
        p.name = dk[i]->name;
        p.read_bps = static_cast<double>(dk[i]->read_bytes) / secs;
        p.write_bps = static_cast<double>(dk[i]->write_bytes) / secs;
        p.mean_latency_ms = dk[i]->ios ? dk[i]->latency_sum_ms / static_cast<double>(dk[i]->ios) : 0.0;
        p.max_latency_ms = dk[i]->max_latency_ms;
    }
    std::vector<const ProcessFaults*> fk;
    for (const auto& [pid, f] : tick_.faults)
        if (pid != kOtherPid) fk.push_back(&f);
    std::sort(fk.begin(), fk.end(), [](const ProcessFaults* a, const ProcessFaults* b) { return a->faults > b->faults; });
    for (std::size_t i = 0; i < fk.size() && i < half; ++i) {
        auto& p = procs[fk[i]->pid];
        p.pid = fk[i]->pid;
        if (p.name.empty()) p.name = fk[i]->name;
        p.hard_faults_per_sec = static_cast<double>(fk[i]->faults) / secs;
    }
    // Faults of processes that made the disk list but not the fault list.
    for (auto& [pid, p] : procs)
        if (p.hard_faults_per_sec == 0.0)
            if (auto it = tick_.faults.find(pid); it != tick_.faults.end())
                p.hard_faults_per_sec = static_cast<double>(it->second.faults) / secs;
    s.kernel_processes.clear();
    for (auto& [pid, p] : procs) s.kernel_processes.push_back(std::move(p));

    tick_.clear();
    tick_.start_ms = now;
}

// ---------------------------------------------------------------------------------------------
void SelfProtection::reset() noexcept {
    points_.clear();
    cost_ = 0.0;
    have_lost_ = false;
    last_lost_ = 0;
    losing_since_ = -1;
}

SelfProtection::Verdict SelfProtection::update(std::int64_t now, std::uint64_t cpu_us, std::uint64_t lost) {
    std::int64_t prev_t = points_.empty() ? now : points_.back().t;
    if (!points_.empty() && (now < points_.back().t || cpu_us < points_.back().cpu)) {
        reset();  // clock went back or the counter restarted
        prev_t = now;
    }
    points_.push_back({now, cpu_us});
    // Keep one point at or before the window start as the baseline.
    while (points_.size() > 2 && points_[1].t <= now - cfg_.cost_window_ms) points_.pop_front();
    const Point& f = points_.front();
    const Point& b = points_.back();
    const std::int64_t span = b.t - f.t;
    cost_ = span > 0 ? (static_cast<double>(b.cpu - f.cpu) / 1000.0) / static_cast<double>(span) * 100.0 : 0.0;

    if (have_lost_ && lost < last_lost_) losing_since_ = -1;  // session restarted
    if (have_lost_ && lost > last_lost_) {
        if (losing_since_ < 0) losing_since_ = prev_t;
    } else if (have_lost_) {
        losing_since_ = -1;
    }
    have_lost_ = true;
    last_lost_ = lost;

    if (span >= cfg_.cost_window_ms && cost_ > cfg_.max_cost_percent) return Verdict::TooExpensive;
    if (losing_since_ >= 0 && now - losing_since_ >= cfg_.loss_window_ms) return Verdict::LosingEvents;
    return Verdict::Ok;
}

const char* to_string(SelfProtection::Verdict v) noexcept {
    switch (v) {
        case SelfProtection::Verdict::Ok: return "ok";
        case SelfProtection::Verdict::TooExpensive: return "too_expensive";
        case SelfProtection::Verdict::LosingEvents: return "losing_events";
    }
    return "ok";
}

// ---------------------------------------------------------------------------------------------
json::Value etw_status_json(const EtwStatus& st) {
    json::Value o = json::Value::object();
    o.set("running", st.running);
    json::Value f = json::Value::array();
    for (auto& n : flag_names(st.running ? st.flags : 0)) f.push_back(std::move(n));
    o.set("flags", std::move(f));
    o.set("eventsLost", st.events_lost);
    o.set("costPercent", round_to(st.cost_percent, 0.01));
    if (!st.reason.empty()) o.set("reason", st.reason);
    o.set("enabled", st.enabled);
    o.set("level", st.level);
    o.set("events", st.events);
    return o;
}

json::Value driver_json(const DriverStats& d) {
    json::Value o = json::Value::object();
    o.set("name", d.name);
    o.set("dpcCount", d.dpc_count);
    o.set("isrCount", d.isr_count);
    o.set("totalUs", round_to(d.total_us(), 0.1));
    o.set("maxUs", round_to(d.max_us, 0.1));
    json::Value h = json::Value::array();
    for (auto c : d.histogram) h.push_back(c);
    o.set("histogram", std::move(h));
    const std::string desc = driver_description(d.name);
    if (!desc.empty()) o.set("description", desc);
    return o;
}

json::Value latency_json(const EtwStatus& st, const LatencyView& v) {
    json::Value o = json::Value::object();
    o.set("status", etw_status_json(st));
    json::Value sys = json::Value::object();
    sys.set("dpcPercent", round_to(v.dpc_percent, 0.001));
    sys.set("isrPercent", round_to(v.isr_percent, 0.001));
    sys.set("worstDpcUs", round_to(v.worst_dpc_us, 0.1));
    sys.set("worstIsrUs", round_to(v.worst_isr_us, 0.1));
    o.set("system", std::move(sys));
    json::Value ds = json::Value::array();
    for (const auto& d : v.drivers) ds.push_back(driver_json(d));
    o.set("drivers", std::move(ds));
    return o;
}

json::Value disk_activity_json(const std::vector<DiskProcessView>& v) {
    json::Value arr = json::Value::array();
    for (const auto& p : v) {
        json::Value o = json::Value::object();
        o.set("pid", p.pid);
        o.set("name", p.name);
        o.set("readBps", round_to(p.read_bps, 1.0));
        o.set("writeBps", round_to(p.write_bps, 1.0));
        o.set("ios", p.ios);
        o.set("meanLatencyMs", round_to(p.mean_latency_ms, 0.01));
        o.set("maxLatencyMs", round_to(p.max_latency_ms, 0.01));
        arr.push_back(std::move(o));
    }
    json::Value o = json::Value::object();
    o.set("processes", std::move(arr));
    return o;
}

json::Value hard_faults_json(const std::vector<FaultProcessView>& v) {
    json::Value arr = json::Value::array();
    for (const auto& p : v) {
        json::Value o = json::Value::object();
        o.set("pid", p.pid);
        o.set("name", p.name);
        o.set("faultsPerSec", round_to(p.faults_per_sec, 0.01));
        arr.push_back(std::move(o));
    }
    json::Value o = json::Value::object();
    o.set("processes", std::move(arr));
    return o;
}

}  // namespace opensweep::kstats
