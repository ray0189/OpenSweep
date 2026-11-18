// System metrics via documented Win32 APIs:
//   GlobalMemoryStatusEx, GetPerformanceInfo, GetSystemTimes (CPU %), GetProcessMemoryInfo
//   and GetProcessTimes (this process). Read-only: nothing here changes system state.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "commands.hpp"

namespace opensweep::win {

// Samples system CPU utilisation once per second on a background thread.
class CpuSampler {
public:
    CpuSampler() = default;
    ~CpuSampler();
    CpuSampler(const CpuSampler&) = delete;
    CpuSampler& operator=(const CpuSampler&) = delete;

    void start();
    void stop();  // joins the thread; safe to call more than once
    // Percent busy (0..100) over the last completed interval; 0 until the first interval ends.
    double percent() const;

private:
    void run();
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    double percent_ = 0.0;
    std::thread thread_;
};

class WinSystemProvider final : public commands::SystemProvider {
public:
    WinSystemProvider(CpuSampler& cpu, const commands::TopologyInfo& topology) : cpu_(cpu), topology_(topology) {}
    bool metrics(commands::MetricsSnapshot& out) override;
    bool topology(commands::TopologyInfo& out) override;
    bool self_process(commands::SelfProcessInfo& out) override;
    std::uint32_t pid() override;

private:
    CpuSampler& cpu_;
    commands::TopologyInfo topology_;  // detected once at start-up
};

}  // namespace opensweep::win
