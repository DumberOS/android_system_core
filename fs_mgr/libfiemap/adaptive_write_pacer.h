// Copyright (C) 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace android::fiemap {

// PSI totals are cumulative microseconds. Missing/invalid metrics are optional,
// never interpreted as a healthy system.
struct WritePressure {
    std::optional<uint64_t> io_full_us;
    std::optional<uint64_t> memory_some_us;
    std::optional<uint64_t> memory_full_us;
    std::optional<uint64_t> available_kib;
    std::optional<uint64_t> total_kib;
};

WritePressure ParseWritePressure(const std::string& io, const std::string& memory,
                                 const std::string& meminfo);
WritePressure ReadWritePressure();

class AdaptiveWritePacer {
  public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    struct Hooks {
        std::function<Clock::time_point()> now;
        std::function<void(Duration)> sleep;
        std::function<WritePressure()> read;
    };
    struct Status {
        double mib_per_second = 2;
        bool paused = false;
        std::optional<double> io_full_percent;
        std::optional<double> memory_some_percent;
        std::optional<double> memory_full_percent;
        std::optional<uint64_t> available_kib;
        double batch_seconds = 0;
        uint64_t bytes_written = 0;
        const char* action = "start";
        const char* reason = "initial";
    };
    using Reporter = std::function<void(const Status&)>;

    explicit AdaptiveWritePacer(Reporter report = {});
    AdaptiveWritePacer(Hooks hooks, Reporter report = {});
    // Polls cancellation at most 100ms apart while sleeping. A blocked write or
    // fsync must finish first. On failure errno is ECANCELED or ETIMEDOUT.
    bool BeforeWrite(const std::function<bool()>& keep_going);
    // Called after BOTH writing and flushing; time spent doing I/O counts
    // towards pacing. Never accumulate credit for a subsequent burst.
    void AfterWrite(uint64_t bytes);
    const Status& status() const { return status_; }

  private:
    void Sample(Clock::time_point now);
    Hooks hooks_;
    Reporter report_;
    Status status_;
    WritePressure previous_;
    std::optional<Clock::time_point> sampled_at_;
    std::optional<Clock::time_point> paused_at_;
    std::optional<Clock::time_point> reduced_at_;
    Clock::time_point next_write_{};
    Clock::time_point batch_started_{};
    Clock::time_point last_report_{};
    double max_batch_seconds_ = 0;
    unsigned healthy_samples_ = 0;
    unsigned pressured_samples_ = 0;
};

}  // namespace android::fiemap
