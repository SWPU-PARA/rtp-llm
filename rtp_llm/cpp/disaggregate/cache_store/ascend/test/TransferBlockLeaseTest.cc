// Copyright (c) 2026 Alibaba Cloud.
// SPDX-License-Identifier: Apache-2.0
#include "rtp_llm/cpp/cache/TransferBlockLease.h"
#include "rtp_llm/cpp/disaggregate/cache_store/RequestBlockBuffer.h"
#include "rtp_llm/cpp/disaggregate/cache_store/ascend/HixlManifest.h"
#include "rtp_llm/cpp/disaggregate/cache_store/ascend/HixlReadWorker.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <future>

namespace rtp_llm {
namespace {

class TransferBlockLeaseTest: public ::testing::Test {
protected:
    void SetUp() override {
        BlockPoolConfig config;
        config.block_num = 8;
        pool             = std::make_shared<BlockPool>(config);
        // Exercise the real page allocator/reference implementation without
        // allocating tensor storage or making a device request.
        pool->initFreeBlocks();
    }
    BlockPoolPtr pool;
};

TEST_F(TransferBlockLeaseTest, RequestCancellationDoesNotRecycleInFlightPage) {
    auto ids   = pool->malloc(1);
    auto lease = std::make_shared<TransferBlockLease>(pool, ids);
    pool->requestFree(ids);
    EXPECT_EQ(pool->freeBlocksNum(), 6);
    EXPECT_EQ(pool->connectorRefBlocksNum(), 1);
    EXPECT_EQ(pool->malloc(6).size(), 6);
    EXPECT_TRUE(pool->malloc(1).empty());
    lease.reset();
    EXPECT_EQ(pool->malloc(1), ids);
}

TEST_F(TransferBlockLeaseTest, PublishedAndCopiedDescriptorsRetainLease) {
    auto ids    = pool->malloc(1);
    auto lease  = std::make_shared<TransferBlockLease>(pool, ids);
    auto buffer = std::make_shared<RequestBlockBuffer>("request");
    pool->requestFree(ids);
    int calls = 0;
    ASSERT_TRUE(buffer->setWatchFunc([&](bool ok, const auto& blocks) {
        if (!blocks.empty()) {
            EXPECT_TRUE(ok);
            EXPECT_EQ(blocks.size(), 1);
            EXPECT_EQ(blocks.front()->addr.get(), pool.get());
            EXPECT_EQ(pool->freeBlocksNum(), 6);
            ++calls;
        }
    }));
    buffer->addBlock("kv", std::shared_ptr<void>(lease, pool.get()), 16, true, true);
    auto copy = std::make_shared<BlockBuffer>(*buffer->getBlock("kv"));
    lease.reset();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(pool->connectorRefBlocksNum(), 1);
    buffer.reset();
    EXPECT_EQ(pool->freeBlocksNum(), 6);
    copy.reset();
    EXPECT_EQ(pool->freeBlocksNum(), 7);
}

TEST_F(TransferBlockLeaseTest, IndependentReadersReleaseIndependently) {
    auto ids    = pool->malloc(1);
    auto first  = std::make_shared<TransferBlockLease>(pool, ids);
    auto second = std::make_shared<TransferBlockLease>(pool, ids);
    pool->requestFree(ids);
    first.reset();
    EXPECT_EQ(pool->freeBlocksNum(), 6);
    second.reset();
    EXPECT_EQ(pool->freeBlocksNum(), 7);
}

TEST_F(TransferBlockLeaseTest, RepeatedLayerIdsDoNotLeakReferences) {
    auto ids   = pool->malloc(1);
    auto lease = std::make_shared<TransferBlockLease>(pool, BlockIndicesType{ids[0], ids[0], ids[0]});
    pool->requestFree(ids);
    lease.reset();
    EXPECT_EQ(pool->freeBlocksNum(), 7);
    EXPECT_EQ(pool->connectorRefBlocksNum(), 0);
}

TEST_F(TransferBlockLeaseTest, InvalidPhysicalIdsAreRejectedBeforePinning) {
    for (auto id : {-1, 0, 8}) {
        EXPECT_ANY_THROW(std::make_shared<TransferBlockLease>(pool, BlockIndicesType{id}));
        EXPECT_EQ(pool->freeBlocksNum(), 7);
        EXPECT_EQ(pool->connectorRefBlocksNum(), 0);
    }
}

TEST_F(TransferBlockLeaseTest, ReaderGrantOutlivesPublicationAndRequest) {
    auto ids   = pool->malloc(1);
    auto block = std::make_shared<BlockBuffer>(
        "kv", std::shared_ptr<void>(std::make_shared<TransferBlockLease>(pool, ids), pool.get()), 16, true, true);
    HixlSourceGrants grants;
    EXPECT_FALSE(grants.acquireReserved("reader", 1, {block}));
    ASSERT_TRUE(grants.reserve("reader", "engine", 1));
    ASSERT_TRUE(grants.retire("reader", "engine", 1));
    EXPECT_FALSE(grants.acquireReserved("reader", 1, {block}));
    EXPECT_FALSE(grants.reserve("reader", "engine", 1));
    ASSERT_TRUE(grants.reserve("reader", "engine", 2));
    ASSERT_TRUE(grants.acquireReserved("reader", 2, {block}));
    EXPECT_FALSE(grants.acquireReserved("reader", 2, {block}));
    pool->requestFree(ids);
    block.reset();
    EXPECT_TRUE(grants.retire("other-reader", "other-engine", 2));
    EXPECT_FALSE(grants.retire("reader", "wrong-engine", 2));
    EXPECT_TRUE(grants.retire("reader", "engine", 1));
    EXPECT_EQ(pool->freeBlocksNum(), 6);
    EXPECT_TRUE(grants.retire("reader", "engine", 2));
    EXPECT_TRUE(grants.retire("reader", "engine", 2));
    EXPECT_EQ(pool->freeBlocksNum(), 7);
}

class MockReadDriver: public HixlReadDriver {
public:
    MOCK_METHOD(bool, activate, (), (override));
    MOCK_METHOD(bool, connect, (const std::string&), (override));
    MOCK_METHOD(HixlLinkState, connection, (const std::string&), (override));
    MOCK_METHOD(bool, read, (const std::string&, const std::vector<HixlTransferSlice>&, void*&), (override));
    MOCK_METHOD(HixlReadState, poll, (void*), (override));
    MOCK_METHOD(bool, disconnect, (const std::string&), (override));
};

TEST_F(TransferBlockLeaseTest, TimedOutReadRetainsPageUntilPhysicalDrain) {
    using namespace testing;
    using namespace std::chrono_literals;
    std::promise<void>           waiting, drained;
    auto                         at_drain = waiting.get_future(), allow_drain = drained.get_future();
    std::promise<HixlReadResult> done;
    auto                         result = done.get_future();
    auto                         driver = std::make_unique<StrictMock<MockReadDriver>>();
    EXPECT_CALL(*driver, activate()).WillOnce(Return(true));
    EXPECT_CALL(*driver, connect("P")).WillOnce(Return(true));
    EXPECT_CALL(*driver, read("P", _, _)).WillOnce(DoAll(SetArgReferee<2>(pool.get()), Return(true)));
    EXPECT_CALL(*driver, poll(_)).WillRepeatedly(Return(HixlReadState::WAITING));
    EXPECT_CALL(*driver, disconnect("P")).WillOnce(Return(true));
    EXPECT_CALL(*driver, connection("P"))
        .WillOnce(Return(HixlLinkState::READY))
        .WillOnce(Return(HixlLinkState::CONNECTING))
        .WillOnce([&](const auto&) {
            waiting.set_value();
            allow_drain.wait();  // Keep the READ physically in flight until the assertions below.
            return HixlLinkState::CLOSED;
        });
    HixlReadProgress worker(std::move(driver), 1);
    ASSERT_TRUE(worker.start());
    auto ids = pool->malloc(1);
    ASSERT_TRUE(worker.submit({"P",
                               {{1, 2, 1}},
                               std::chrono::steady_clock::now() + 1s,
                               std::make_shared<TransferBlockLease>(pool, ids),
                               [&](auto status) {
                                   EXPECT_EQ(pool->freeBlocksNum(), 6);
                                   done.set_value(status);
                               }}));
    pool->requestFree(ids);
    EXPECT_EQ(at_drain.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(pool->freeBlocksNum(), 6);
    EXPECT_EQ(result.wait_for(0s), std::future_status::timeout);
    drained.set_value();
    worker.stop();
    ASSERT_EQ(result.wait_for(0s), std::future_status::ready);
    EXPECT_EQ(result.get(), HixlReadResult::TIMEOUT);
    EXPECT_EQ(pool->freeBlocksNum(), 7);
}

}  // namespace
}  // namespace rtp_llm
