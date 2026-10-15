// Copyright (C) 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#include "adaptive_write_pacer.h"

#include <cerrno>
#include <gtest/gtest.h>

namespace android::fiemap {
namespace {
using namespace std::chrono_literals;
constexpr uint64_t MiB = 1024 * 1024;

class PacerTest : public testing::Test {
  protected:
    AdaptiveWritePacer::Clock::time_point now{};
    AdaptiveWritePacer::Clock::time_point last_read{};
    WritePressure metrics{0, 0, 0, 1024 * 1024, 4 * 1024 * 1024};
    double io = 0, memory = 0, memory_full = 0;
    std::function<void()> on_sleep;
    AdaptiveWritePacer pacer{{
            [&] { return now; },
            [&](auto delay) {
                EXPECT_LE(delay, 100ms);
                now += delay;
                if (on_sleep) on_sleep();
            },
            [&] {
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_read).count();
                if (metrics.io_full_us) *metrics.io_full_us += us * io;
                if (metrics.memory_some_us) *metrics.memory_some_us += us * memory;
                if (metrics.memory_full_us) *metrics.memory_full_us += us * memory_full;
                last_read = now;
                return metrics;
            }}};

    void Batch(std::chrono::milliseconds duration = 10ms) {
        ASSERT_TRUE(pacer.BeforeWrite({}));
        now += duration;
        pacer.AfterWrite(MiB);
    }
    void RunFor(std::chrono::seconds duration) {
        auto end = now + duration;
        while (now < end) Batch();
    }
};

TEST_F(PacerTest, CountsFlushTimeAndPacesFirstBatch) {
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 2);
    Batch(100ms);
    ASSERT_TRUE(pacer.BeforeWrite({}));
    EXPECT_EQ(now.time_since_epoch(), 500ms);
}

TEST_F(PacerTest, IsolatedSlowFlushDoesNotCutRateOrCreateCatchupBurst) {
    Batch(2000ms);
    auto before = now;
    Batch();
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 2);
    ASSERT_TRUE(pacer.BeforeWrite({}));
    EXPECT_GE(now - before, 500ms);
}

TEST_F(PacerTest, RampsSlowlyAndCapsMaximum) {
    RunFor(4s);
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 2);
    RunFor(200s);
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 16);
}

TEST_F(PacerTest, IoPressureCutsRateAndHonorsMinimum) {
    io = 0.15;
    RunFor(40s);
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 0.5);
    EXPECT_FALSE(pacer.status().paused);
}

TEST_F(PacerTest, MemoryPressureCutsRateWithPlentyOfAvailableMemory) {
    memory = 0.08;
    RunFor(4s);
    EXPECT_LT(pacer.status().mib_per_second, 2);
}

TEST_F(PacerTest, PartialMemoryPressureKeepsWritingBeyondPauseTimeout) {
    memory = 0.4;
    memory_full = 0.005;
    // Long-lived partial stalls must slow writes without stopping progress or
    // triggering the five-minute pause timeout.
    while (now.time_since_epoch() < 6min) {
        ASSERT_TRUE(pacer.BeforeWrite([&] { return now.time_since_epoch() < 7min; }));
        ASSERT_FALSE(pacer.status().paused);
        now += 10ms;
        pacer.AfterWrite(MiB);
    }
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 0.5);
    EXPECT_GE(pacer.status().bytes_written, 180 * MiB);
}

TEST_F(PacerTest, PartialMemoryPressureBelowReductionThresholdHoldsRate) {
    memory = 0.03;
    RunFor(20s);
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 2);
    EXPECT_FALSE(pacer.status().paused);
}

TEST_F(PacerTest, IoPauseRecoversDespitePartialMemoryPressure) {
    memory = 0.3;
    io = 0.4;
    Batch();
    now += 1s;
    bool saw_pause = false;
    on_sleep = [&] {
        saw_pause |= pacer.status().paused;
        if (now.time_since_epoch() >= 3s) io = 0;
    };
    ASSERT_TRUE(pacer.BeforeWrite([&] { return now.time_since_epoch() < 10s; }));
    EXPECT_TRUE(saw_pause);
    EXPECT_GE(now.time_since_epoch(), 5s);
    EXPECT_FALSE(pacer.status().paused);
    EXPECT_STREQ(pacer.status().action, "resume");
}

TEST_F(PacerTest, MemoryFullPauseRequiresFullPressureRecovery) {
    memory = 0.3;
    memory_full = 0.12;
    Batch();
    now += 1s;
    EXPECT_FALSE(pacer.BeforeWrite([&] { return now.time_since_epoch() < 2s; }));
    EXPECT_TRUE(pacer.status().paused);
    EXPECT_STREQ(pacer.status().reason, "memory_full");
    // Falling below the pause threshold alone is insufficient for recovery.
    memory_full = 0.02;
    EXPECT_FALSE(pacer.BeforeWrite([&] { return now.time_since_epoch() < 6s; }));
    EXPECT_TRUE(pacer.status().paused);
    memory_full = 0;
    ASSERT_TRUE(pacer.BeforeWrite([&] { return now.time_since_epoch() < 11s; }));
    EXPECT_GE(now.time_since_epoch(), 8s);
    EXPECT_FALSE(pacer.status().paused);
    EXPECT_STREQ(pacer.status().action, "resume");
}

TEST_F(PacerTest, MemoryReserveScalesWithRam) {
    metrics.total_kib = 3 * 1024 * 1024;
    metrics.available_kib = 350 * 1024;
    Batch();
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 2);
    metrics.total_kib = 4 * 1024 * 1024;
    RunFor(4s);
    EXPECT_LT(pacer.status().mib_per_second, 2);
}

TEST_F(PacerTest, SevereIoPausesUntilSeveralHealthySamples) {
    io = 0.4;
    bool saw_pause = false;
    on_sleep = [&] {
        saw_pause |= pacer.status().paused;
        if (now.time_since_epoch() >= 3s) io = 0;
    };
    RunFor(2s);  // This call must wait through the pause before writing again.
    EXPECT_TRUE(saw_pause);
    EXPECT_GE(now.time_since_epoch(), 5s);
    EXPECT_FALSE(pacer.status().paused);
    EXPECT_LE(pacer.status().mib_per_second, 1);
}

TEST_F(PacerTest, CancellationRemainsResponsiveDuringPause) {
    metrics.available_kib = 1;
    EXPECT_FALSE(pacer.BeforeWrite([&] { return now.time_since_epoch() < 250ms; }));
    EXPECT_EQ(errno, ECANCELED);
    EXPECT_LE(now.time_since_epoch(), 350ms);
}

TEST_F(PacerTest, SustainedSeverePressureTimesOut) {
    metrics.available_kib = 1;
    EXPECT_FALSE(pacer.BeforeWrite({}));
    EXPECT_EQ(errno, ETIMEDOUT);
    EXPECT_EQ(now.time_since_epoch(), 5min);
}

TEST_F(PacerTest, MissingMetricsKeepConservativeRate) {
    metrics = {};
    RunFor(20s);
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 2);
    for (unsigned i = 0; i < 10; ++i) Batch(600ms);
    EXPECT_LT(pacer.status().mib_per_second, 2);
}

TEST_F(PacerTest, ModerateCutsRequireThreeSamplesAndFiveSecondCooldown) {
    io = 0.15;
    ASSERT_TRUE(pacer.BeforeWrite({}));
    for (unsigned second = 1; second <= 8; ++second) {
        now += 1s;
        ASSERT_TRUE(pacer.BeforeWrite({}));
        double expected = second < 3 ? 2 : second < 8 ? 1.5 : 1.125;
        EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, expected) << second;
    }
    EXPECT_STREQ(pacer.status().action, "reduce");
    EXPECT_STREQ(pacer.status().reason, "io_full");
}

TEST_F(PacerTest, PauseDoesNotKeepReducingRateWithoutWrites) {
    Batch();
    io = 0.4;
    now += 1s;
    EXPECT_FALSE(pacer.BeforeWrite([&] { return now.time_since_epoch() < 20s; }));
    EXPECT_EQ(errno, ECANCELED);
    EXPECT_TRUE(pacer.status().paused);
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 1);
    EXPECT_EQ(pacer.status().bytes_written, MiB);
}

TEST_F(PacerTest, SeverePressureBypassesModerateCooldown) {
    io = 0.15;
    ASSERT_TRUE(pacer.BeforeWrite({}));
    for (unsigned i = 0; i < 3; ++i) {
        now += 1s;
        ASSERT_TRUE(pacer.BeforeWrite({}));
    }
    ASSERT_DOUBLE_EQ(pacer.status().mib_per_second, 1.5);
    io = 0.4;
    now += 1s;
    EXPECT_FALSE(pacer.BeforeWrite([&] { return now.time_since_epoch() < 4500ms; }));
    EXPECT_TRUE(pacer.status().paused);
    EXPECT_DOUBLE_EQ(pacer.status().mib_per_second, 0.75);
    EXPECT_STREQ(pacer.status().reason, "io_full");
}

TEST_F(PacerTest, ReportsMemoryFullPauseAndWrittenBytes) {
    Batch();
    memory = 0.12;
    memory_full = 0.11;
    now += 1s;
    EXPECT_FALSE(pacer.BeforeWrite([&] { return now.time_since_epoch() < 1500ms; }));
    EXPECT_TRUE(pacer.status().paused);
    EXPECT_STREQ(pacer.status().reason, "memory_full");
    ASSERT_TRUE(pacer.status().memory_full_percent);
    EXPECT_NEAR(*pacer.status().memory_full_percent, 11, 0.01);
    EXPECT_EQ(pacer.status().bytes_written, MiB);
}

TEST(WritePacerRegressionTest, PeriodicShortSpikesDoNotCollapseThroughput) {
    // 1% baseline plus 120ms full I/O stalls every ten seconds. Previously
    // these isolated samples halved the rate repeatedly: ~24 simulated minutes
    // for 2300 MiB, even though average full I/O pressure was only 2.2%.
    AdaptiveWritePacer::Clock::time_point now{};
    WritePressure metrics{0, 0, 0, 2 * 1024 * 1024, 4 * 1024 * 1024};
    unsigned cuts = 0;
    double previous_rate = 2;
    AdaptiveWritePacer pacer({[&] { return now; }, [&](auto delay) { now += delay; }, [&] {
        uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        metrics.io_full_us = us / 100 + (us / 10000000) * 120000 +
                            std::min<uint64_t>(us % 10000000, 1000000) * 12 / 100;
        return metrics;
    }}, [&](const auto& status) {
        if (status.mib_per_second < previous_rate) ++cuts;
        previous_rate = status.mib_per_second;
    });
    for (unsigned batch = 0; batch < 2300; ++batch) {
        ASSERT_TRUE(pacer.BeforeWrite({}));
        now += 14ms;
        pacer.AfterWrite(MiB);
    }
    EXPECT_EQ(cuts, 0u);
    EXPECT_LT(now.time_since_epoch(), 400s);
    EXPECT_GT(pacer.status().mib_per_second, 8);
    EXPECT_EQ(pacer.status().bytes_written, 2300 * MiB);
}

TEST_F(PacerTest, LosingMetricsAfterRampImmediatelyCapsRate) {
    RunFor(20s);
    EXPECT_GT(pacer.status().mib_per_second, 2);
    metrics.io_full_us.reset();
    RunFor(2s);
    EXPECT_LE(pacer.status().mib_per_second, 2);
}

TEST_F(PacerTest, CounterResetIsNotAnOverflowOrHealthySample) {
    metrics.io_full_us = 100000000;
    RunFor(10s);
    metrics.io_full_us = 0;
    RunFor(1s);
    EXPECT_LE(pacer.status().mib_per_second, 2);
    EXPECT_FALSE(pacer.status().paused);
}

TEST(WritePressureTest, ParsesTotalsAndAvailableRatherThanFreeMemory) {
    auto p = ParseWritePressure("some avg10=99 total=90\nfull avg10=1 total=123\n",
                               "some avg10=2 total=456\nfull avg10=1 total=78\n",
                               "MemTotal: 4000000 kB\nMemFree: 100 kB\nMemAvailable: 800000 kB\n");
    EXPECT_EQ(p.io_full_us, 123u);
    EXPECT_EQ(p.memory_some_us, 456u);
    EXPECT_EQ(p.memory_full_us, 78u);
    EXPECT_EQ(p.available_kib, 800000u);
}

TEST(WritePressureTest, RejectsMalformedAndMissingMetrics) {
    auto p = ParseWritePressure("full total=-1", "some total=123bad\nfull total=18446744073709551616",
                               "MemTotal: 100 kB\nMemAvailable: 200 kB\n");
    EXPECT_FALSE(p.io_full_us);
    EXPECT_FALSE(p.memory_some_us);
    EXPECT_FALSE(p.memory_full_us);
    EXPECT_FALSE(p.available_kib);
    p = ParseWritePressure("", "", "");
    EXPECT_FALSE(p.total_kib);
}
}  // namespace
}  // namespace android::fiemap
