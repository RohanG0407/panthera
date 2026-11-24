//
// Copyright (c) 2018-2019 Ampere Computing. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//

#include "ampere/sc/clock.h"
#include "ampere/sc/testing/test.h"

#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <vector>
#include "ampere/sc/testing/clock_dummy.h"

namespace ampere {
namespace sc {

struct TestRunnable : public Runnable
{
    TestRunnable(Clock& clock) : clock(clock) {}

    bool run() override
    {
        run_history.push_back(clock.get_cycle());
        return false;
    }

    void init() override { init_history.push_back(clock.get_cycle()); }

    Clock& clock;
    std::vector<Cycle> run_history;
    std::vector<Cycle> init_history;
};

struct ThrowingRunnable : public Runnable
{
    bool run() override
    {
        ++runs;
        throw std::runtime_error{"thrown by a test"};
    }

    int runs{0};
};

// Schedules other runnables for the cycle in which it is initialized
struct SchedulingRunnable : public TestRunnable
{
    SchedulingRunnable(Clock& clock, int phase) : TestRunnable(clock) { set_phase(phase); }

    void init() override
    {
        TestRunnable::init();
        for (auto* runnable : to_schedule)
            clock.schedule(runnable, 0);
    }

    std::vector<Runnable*> to_schedule;
};

struct ClockTest : public Test
{};

TEST_F(ClockTest, TickAdvancesClockCycle)
{
    ClockDummy clock;
    EXPECT_EQ(clock.get_cycle(), 0);
    clock.tick();
    EXPECT_EQ(clock.get_cycle(), 1);
}

TEST_F(ClockTest, RunnableRunsWhenScheduled)
{
    ClockDummy clock;
    TestRunnable runnable(clock);
    clock.schedule(&runnable, 1);
    clock.tick();
    clock.tick();
    std::vector<Cycle> expected_history = {1};
    EXPECT_EQ(runnable.run_history, expected_history);
}

TEST_F(ClockTest, RunnableRunsInTimeOrder)
{
    ClockDummy clock;
    TestRunnable runnable(clock);
    clock.schedule(&runnable, 3);
    clock.schedule(&runnable, 2);
    clock.schedule(&runnable, 1);
    for (int i = 0; i < 4; i++)
        clock.tick();
    std::vector<Cycle> expected_history = {1, 2, 3};
    EXPECT_EQ(runnable.run_history, expected_history);
}

TEST_F(ClockTest, RunnableWontRunMoreThanOnceACycle)
{
    ClockDummy clock;
    TestRunnable runnable(clock);
    clock.schedule(&runnable, 1);
    clock.schedule(&runnable, 1);
    clock.tick();
    clock.tick();
    std::vector<Cycle> expected_history = {1};
    EXPECT_EQ(runnable.run_history, expected_history);
}

TEST_F(ClockTest, RunnableInitializes)
{
    ClockDummy clock;
    TestRunnable runnable(clock);
    clock.add_runnable(&runnable);
    clock.tick();
    std::vector<Cycle> expected_history = {0};
    EXPECT_EQ(runnable.init_history, expected_history);
}

TEST_F(ClockTest, RunnableScheduledDuringInitIsInitializedAndRuns)
{
    ClockDummy clock;
    SchedulingRunnable first(clock, 1);
    std::vector<std::unique_ptr<SchedulingRunnable>> others;
    for (int i = 0; i < 100; i++) {
        others.push_back(std::make_unique<SchedulingRunnable>(clock, 1));
        first.to_schedule.push_back(others.back().get());
    }
    clock.add_runnable(&first);
    clock.tick();

    const std::vector<Cycle> expected_history = {0};
    EXPECT_EQ(first.init_history, expected_history);
    EXPECT_EQ(first.run_history, expected_history);
    for (const auto& other : others) {
        EXPECT_EQ(other->init_history, expected_history);
        EXPECT_EQ(other->run_history, expected_history);
    }
}

TEST_F(ClockTest, TickAfterRunnableThrowsGoesOnWithTheRest)
{
    ClockDummy clock;
    TestRunnable before(clock);
    ThrowingRunnable thrower;
    TestRunnable after(clock);
    clock.schedule(&before, 1);
    clock.schedule(&thrower, 1);
    clock.schedule(&after, 1);
    clock.tick();

    EXPECT_THROW(clock.tick(), std::runtime_error);
    EXPECT_EQ(clock.get_cycle(), 1);
    EXPECT_TRUE(after.run_history.empty());

    // The cycle that was cut short finishes: what ran does not run again, and what was left does
    clock.tick();
    EXPECT_EQ(clock.get_cycle(), 2);
    const std::vector<Cycle> expected_history = {1};
    EXPECT_EQ(before.run_history, expected_history);
    EXPECT_EQ(thrower.runs, 1);
    EXPECT_EQ(after.run_history, expected_history);
}

} // namespace sc
} // namespace ampere

/*
 * vim: sw=4:ts=4:et:si:cindent:cino=\:0,(0,g0,t0:
 */
