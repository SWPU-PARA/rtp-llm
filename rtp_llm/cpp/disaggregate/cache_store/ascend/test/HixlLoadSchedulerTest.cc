// Copyright (c) 2026 Alibaba Cloud.
// SPDX-License-Identifier: Apache-2.0
#include "rtp_llm/cpp/disaggregate/cache_store/ascend/HixlReadWorker.h"
#include "rtp_llm/cpp/disaggregate/cache_store/ascend/HixlProtocol.h"

#include <gtest/gtest.h>
#include <future>
#include <memory>

namespace rtp_llm {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

HixlPendingLoad load(std::promise<uint64_t>&          started,
                     std::promise<HixlLoadRejection>& rejected,
                     std::chrono::milliseconds        timeout  = 5s,
                     std::string                      endpoint = "P") {
    return {std::move(endpoint),
            Clock::now() + timeout,
            [&started](uint64_t sequence) { started.set_value(sequence); },
            [&rejected](auto reason) { rejected.set_value(reason); }};
}

TEST(HixlLoadSchedulerTest, FullInflightWindowQueuesUntilSourceRetirement) {
    HixlLoadScheduler queue(1, 2);
    ASSERT_TRUE(queue.start());
    std::promise<uint64_t>          a, b;
    std::promise<HixlLoadRejection> ra, rb;
    auto                            fa = a.get_future(), fb = b.get_future();
    ASSERT_TRUE(queue.submit(load(a, ra)));
    ASSERT_EQ(fa.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(fa.get(), 1);
    ASSERT_TRUE(queue.submit(load(b, rb)));
    EXPECT_EQ(fb.wait_for(20ms), std::future_status::timeout);
    EXPECT_EQ(queue.inflight(), 1);
    EXPECT_EQ(queue.pending(), 1);
    queue.retired("P", 1);
    ASSERT_EQ(fb.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(fb.get(), 2);
    queue.retired("P", 1);  // A repeated acknowledgement must not retire B.
    EXPECT_EQ(queue.inflight(), 1);
    queue.retired("P", 2);
    EXPECT_TRUE(queue.waitForIdle(2s));
}

TEST(HixlLoadSchedulerTest, QueuedDeadlineExpiresWithoutConsumingASequence) {
    HixlLoadScheduler queue(1, 4);
    ASSERT_TRUE(queue.start());
    std::promise<uint64_t>          a, b, c;
    std::promise<HixlLoadRejection> ra, rb, rc;
    auto                            fa = a.get_future(), fb = b.get_future(), fc = c.get_future();
    auto                            expired = rb.get_future();
    ASSERT_TRUE(queue.submit(load(a, ra)));
    ASSERT_EQ(fa.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(queue.submit(load(b, rb, 20ms)));
    ASSERT_EQ(expired.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(expired.get(), HixlLoadRejection::TIMEOUT);
    EXPECT_EQ(fb.wait_for(0ms), std::future_status::timeout);
    ASSERT_TRUE(queue.submit(load(c, rc)));
    queue.retired("P", fa.get());
    ASSERT_EQ(fc.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(fc.get(), 2);
    queue.retired("P", 2);
    EXPECT_TRUE(queue.waitForIdle(2s));
}

TEST(HixlLoadSchedulerTest, PendingBoundAndShutdownReleaseOnlyUnissuedWork) {
    HixlLoadScheduler queue(1, 1);
    ASSERT_TRUE(queue.start());
    std::promise<uint64_t>          a, b, c;
    std::promise<HixlLoadRejection> ra, rb, rc;
    auto                            fa       = a.get_future();
    auto                            rejected = rb.get_future();
    ASSERT_TRUE(queue.submit(load(a, ra)));
    ASSERT_EQ(fa.wait_for(2s), std::future_status::ready);
    auto               lease   = std::make_shared<int>(1);
    std::weak_ptr<int> alive   = lease;
    auto               waiting = load(b, rb);
    waiting.start              = [lease](uint64_t) { ADD_FAILURE() << "queued load started during stop"; };
    ASSERT_TRUE(queue.submit(std::move(waiting)));
    lease.reset();
    EXPECT_FALSE(alive.expired());
    EXPECT_FALSE(queue.submit(load(c, rc)));
    queue.stop();
    ASSERT_EQ(rejected.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(rejected.get(), HixlLoadRejection::STOPPED);
    EXPECT_TRUE(alive.expired());
    EXPECT_EQ(queue.inflight(), 1);
    EXPECT_FALSE(queue.waitForIdle(1ms));
    EXPECT_FALSE(queue.submit(load(c, rc)));
    queue.retired("P", fa.get());
    EXPECT_TRUE(queue.waitForIdle(2s));
}

TEST(HixlLoadSchedulerTest, InvalidBoundsAndUnstartedAdmissionAreRejected) {
    EXPECT_THROW(HixlLoadScheduler(0, 1), std::invalid_argument);
    EXPECT_THROW(HixlLoadScheduler(kHixlTransferWindow + 1, 1), std::invalid_argument);
    EXPECT_THROW(HixlLoadScheduler(1, 0), std::invalid_argument);
    HixlLoadScheduler               queue(1, 1);
    std::promise<uint64_t>          a;
    std::promise<HixlLoadRejection> ra;
    EXPECT_FALSE(queue.submit(load(a, ra)));
    ASSERT_TRUE(queue.start());
    EXPECT_FALSE(queue.start());
}
}  // namespace
}  // namespace rtp_llm
