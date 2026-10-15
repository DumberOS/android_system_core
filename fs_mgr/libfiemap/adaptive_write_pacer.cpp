// Copyright (C) 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#include "adaptive_write_pacer.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

namespace android::fiemap {
namespace {
using namespace std::chrono_literals;

std::optional<uint64_t> Number(const std::string& text) {
    uint64_t value;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return {};
    return value;
}

std::optional<uint64_t> PsiTotal(const std::string& text, const std::string& kind) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string field;
        if (!(fields >> field) || field != kind) continue;
        while (fields >> field) {
            if (field.compare(0, 6, "total=") == 0) return Number(field.substr(6));
        }
    }
    return {};
}

std::optional<uint64_t> MemValue(const std::string& text, const std::string& key) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string name, value, unit;
        if (fields >> name >> value >> unit && name == key && unit == "kB") {
            return Number(value);
        }
    }
    return {};
}

std::string Read(const char* path) {
    std::ifstream input(path);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::optional<double> Percent(std::optional<uint64_t> before, std::optional<uint64_t> after,
                              double seconds) {
    if (!before || !after || *after < *before || seconds <= 0) return {};
    return std::min(100.0, (*after - *before) / (seconds * 10000.0));
}
}  // namespace

WritePressure ParseWritePressure(const std::string& io, const std::string& memory,
                                 const std::string& meminfo) {
    WritePressure p;
    p.io_full_us = PsiTotal(io, "full");
    p.memory_some_us = PsiTotal(memory, "some");
    p.memory_full_us = PsiTotal(memory, "full");
    p.available_kib = MemValue(meminfo, "MemAvailable:");
    p.total_kib = MemValue(meminfo, "MemTotal:");
    if (!p.total_kib || *p.total_kib == 0 ||
        (p.available_kib && *p.available_kib > *p.total_kib)) {
        p.available_kib.reset();
        p.total_kib.reset();
    }
    return p;
}

WritePressure ReadWritePressure() {
    return ParseWritePressure(Read("/proc/pressure/io"), Read("/proc/pressure/memory"),
                              Read("/proc/meminfo"));
}

AdaptiveWritePacer::AdaptiveWritePacer(Reporter report)
    : AdaptiveWritePacer({[] { return Clock::now(); },
                          [](Duration delay) { std::this_thread::sleep_for(delay); },
                          ReadWritePressure}, std::move(report)) {}

AdaptiveWritePacer::AdaptiveWritePacer(Hooks hooks, Reporter report)
    : hooks_(std::move(hooks)), report_(std::move(report)) {}

void AdaptiveWritePacer::Sample(Clock::time_point now) {
    if (sampled_at_ && now - *sampled_at_ < 1s) return;
    auto p = hooks_.read();
    status_.io_full_percent.reset();
    status_.memory_some_percent.reset();
    status_.memory_full_percent.reset();
    if (sampled_at_) {
        double seconds = std::chrono::duration<double>(now - *sampled_at_).count();
        status_.io_full_percent = Percent(previous_.io_full_us, p.io_full_us, seconds);
        status_.memory_some_percent = Percent(previous_.memory_some_us, p.memory_some_us, seconds);
        status_.memory_full_percent = Percent(previous_.memory_full_us, p.memory_full_us, seconds);
    }
    previous_ = p;
    bool first = !sampled_at_;
    sampled_at_ = now;
    status_.available_kib = p.available_kib;
    status_.batch_seconds = max_batch_seconds_;

    // Starting tuning values, deliberately conservative. PSI includes our own
    // waits, so require substantial full I/O stalls rather than zero pressure.
    uint64_t reserve = std::max<uint64_t>(256 * 1024, p.total_kib.value_or(0) / 10);
    bool low_memory = p.available_kib && *p.available_kib < reserve;
    bool severe = (p.available_kib && *p.available_kib < reserve / 2) ||
                  status_.io_full_percent.value_or(0) >= 25 ||
                  status_.memory_full_percent.value_or(0) >= 10;
    bool pressured = low_memory || status_.io_full_percent.value_or(0) >= 10 ||
                     status_.memory_some_percent.value_or(0) >= 5 ||
                     max_batch_seconds_ >= 0.5;
    bool complete = status_.io_full_percent && status_.memory_some_percent &&
                    status_.memory_full_percent && p.available_kib && p.total_kib;
    bool recovered = !low_memory && (!p.available_kib || *p.available_kib >= reserve * 5 / 4) &&
                     status_.io_full_percent.value_or(0) < 5 &&
                     status_.memory_full_percent.value_or(0) < 1 && max_batch_seconds_ < 0.25;
    // Partial memory stalls can persist even while we write nothing. Let them
    // hold or gradually reduce the rate, but neither start nor prolong a pause.
    // Still require low partial pressure before increasing the rate.
    bool healthy = recovered && status_.memory_some_percent.value_or(0) < 2;
    double old_rate = status_.mib_per_second;
    bool was_paused = status_.paused;
    const char* pressure_reason = low_memory ? "low_available_memory"
                                 : status_.io_full_percent.value_or(0) >= 10 ? "io_full"
                                 : status_.memory_some_percent.value_or(0) >= 5 ? "memory_some"
                                 : status_.memory_full_percent.value_or(0) >= 10 ? "memory_full"
                                 : "slow_batch";
    status_.action = "hold";
    status_.reason = "waiting_for_healthy_samples";
    if (severe) {
        // One cut per pause episode. Background pressure can persist while we
        // write nothing; repeatedly cutting then would only delay recovery.
        if (!status_.paused) {
            status_.mib_per_second = std::max(0.5, status_.mib_per_second / 2);
            reduced_at_ = now;
            paused_at_ = now;
            status_.action = "pause";
        } else {
            status_.action = "paused";
        }
        status_.paused = true;
        status_.reason = p.available_kib && *p.available_kib < reserve / 2 ? "low_available_memory"
                         : status_.io_full_percent.value_or(0) >= 25 ? "io_full"
                         : "memory_full";
        pressured_samples_ = 0;
        healthy_samples_ = 0;
    } else if (status_.paused) {
        status_.action = "paused";
        status_.reason = pressured ? pressure_reason : "waiting_for_recovery";
        if (recovered && !first) {
            if (++healthy_samples_ >= 3) {
                status_.paused = false;
                paused_at_.reset();
                healthy_samples_ = 0;
                status_.action = "resume";
                status_.reason = "pressure_recovered";
            }
        } else {
            healthy_samples_ = 0;
        }
        pressured_samples_ = 0;
    } else if (pressured) {
        healthy_samples_ = 0;
        // Isolated ~100ms I/O stalls can exceed 10% of a one-second sample.
        // Require three consecutive pressured samples and allow at most one
        // moderate cut every five seconds. Severe pressure bypasses this gate.
        pressured_samples_ = std::min(3u, pressured_samples_ + 1);
        status_.reason = pressure_reason;
        if (pressured_samples_ >= 3 && (!reduced_at_ || now - *reduced_at_ >= 5s)) {
            status_.mib_per_second = std::max(0.5, status_.mib_per_second * 0.75);
            reduced_at_ = now;
            pressured_samples_ = 0;
            status_.action = "reduce";
        }
    } else if (healthy && !first) {
        pressured_samples_ = 0;
        ++healthy_samples_;
        if (complete && healthy_samples_ >= 5) {
            status_.mib_per_second = std::min(16.0, status_.mib_per_second + 0.5);
            healthy_samples_ = 0;
            status_.action = "increase";
            status_.reason = "sustained_headroom";
        }
    } else {
        pressured_samples_ = 0;
        healthy_samples_ = 0;
    }
    // Missing metrics or counter resets must not leave us at a previously high rate.
    if (!complete) {
        double before_cap = status_.mib_per_second;
        status_.mib_per_second = std::min(2.0, status_.mib_per_second);
        if (!status_.paused) healthy_samples_ = 0;
        if (status_.mib_per_second < before_cap && !severe) {
            status_.action = "cap";
            status_.reason = "missing_metrics";
        }
    }
    if (first && !severe) {
        status_.action = "start";
        status_.reason = "initial";
    }
    max_batch_seconds_ = 0;
    if (report_ && (first || old_rate != status_.mib_per_second || was_paused != status_.paused ||
                    now - last_report_ >= 10s)) {
        report_(status_);
        last_report_ = now;
    }
}

bool AdaptiveWritePacer::BeforeWrite(const std::function<bool()>& keep_going) {
    for (;;) {
        if (keep_going && !keep_going()) {
            errno = ECANCELED;
            return false;
        }
        auto now = hooks_.now();
        Sample(now);
        if (paused_at_ && now - *paused_at_ >= 5min) {
            errno = ETIMEDOUT;
            return false;
        }
        if (!status_.paused && now >= next_write_) {
            batch_started_ = now;
            return true;
        }
        auto delay = std::chrono::duration_cast<Duration>(100ms);
        if (!status_.paused) delay = std::min(delay, next_write_ - now);
        hooks_.sleep(delay);
    }
}

void AdaptiveWritePacer::AfterWrite(uint64_t bytes) {
    auto now = hooks_.now();
    status_.bytes_written += bytes;
    max_batch_seconds_ = std::max(max_batch_seconds_,
                                 std::chrono::duration<double>(now - batch_started_).count());
    auto interval = std::chrono::duration<double>(bytes / (status_.mib_per_second * 1024 * 1024));
    next_write_ = std::max(now, batch_started_ + std::chrono::duration_cast<Duration>(interval));
}
}  // namespace android::fiemap
