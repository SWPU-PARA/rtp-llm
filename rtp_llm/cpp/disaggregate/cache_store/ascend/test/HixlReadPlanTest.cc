// Copyright (c) 2026 Alibaba Cloud.
// SPDX-License-Identifier: Apache-2.0
#include "rtp_llm/cpp/disaggregate/cache_store/ascend/HixlManifest.h"
#include "rtp_llm/cpp/disaggregate/cache_store/ascend/HixlProtocol.h"

#include <gtest/gtest.h>
#include <array>
#include <limits>
#include <stdexcept>

namespace rtp_llm {
namespace {

class ReadPlanMemory: public MemoryUtil {
public:
    bool regUserMr(void*, uint64_t, bool, uint64_t) override {
        return true;
    }
    bool deregUserMr(void*, bool) override {
        return true;
    }
    bool isMemoryMr(void*, uint64_t, bool, bool) override {
        return registered;
    }
    bool findMemoryMr(void* out, void*, uint64_t, bool, bool) override {
        *static_cast<HixlMemoryRegion*>(out) = {0, 0, 7};
        return registered;
    }
    bool isRdmaMode() override {
        return false;
    }
    bool registered = true;
};

class HixlReadPlanTest: public ::testing::Test {
protected:
    void SetUp() override {
        manifest.set_status(hixl_proto::OK);
        manifest.set_engine_id("source-engine");
        allocation = std::make_shared<std::array<uint8_t, 32>>();
        for (int index : {1, 0}) {
            auto key   = std::to_string(index);
            auto block = std::make_shared<BlockBuffer>(
                key, std::shared_ptr<void>(allocation, allocation->data() + index * 16), 16, true, true);
            destination.addBlock(block);
            auto* remote = manifest.add_blocks();
            remote->set_key(key);
            remote->set_bytes(16);
            remote->set_address(0x1000 + index * 16);
            remote->set_region_id(1);
            remote->set_region_address(0x1000);
            remote->set_region_bytes(32);
        }
    }
    std::vector<HixlTransferSlice> plan() {
        return makeHixlReadPlan(destination, manifest, memory);
    }
    RequestBlockBuffer                       destination{"request"};
    std::shared_ptr<std::array<uint8_t, 32>> allocation;
    hixl_proto::ManifestResponse             manifest;
    ReadPlanMemory                           memory;
};

TEST_F(HixlReadPlanTest, PreservesBlockGranularityAndManifestOrder) {
    const auto slices = plan();
    ASSERT_EQ(slices.size(), 2);
    EXPECT_EQ(slices[0].local_address, reinterpret_cast<uintptr_t>(allocation->data() + 16));
    EXPECT_EQ(slices[0].remote_address, 0x1010);
    EXPECT_EQ(slices[0].bytes, 16);
}

TEST_F(HixlReadPlanTest, RejectsUnknownDuplicateOrMissingKeys) {
    auto original = manifest;
    manifest.mutable_blocks(0)->set_key("unknown");
    EXPECT_THROW(plan(), std::invalid_argument);
    manifest = original;
    manifest.mutable_blocks(0)->set_key(manifest.blocks(1).key());
    EXPECT_THROW(plan(), std::invalid_argument);
    manifest = original;
    manifest.mutable_blocks()->RemoveLast();
    EXPECT_THROW(plan(), std::invalid_argument);
}

TEST_F(HixlReadPlanTest, RejectsLengthAndRegistrationMismatch) {
    auto original = manifest;
    manifest.mutable_blocks(0)->set_bytes(8);
    EXPECT_THROW(plan(), std::invalid_argument);
    manifest = original;
    manifest.mutable_blocks(0)->set_region_bytes(64);
    EXPECT_THROW(plan(), std::invalid_argument);
}

TEST_F(HixlReadPlanTest, RejectsOutOfBoundsAndWrappingRemoteRegions) {
    auto original = manifest;
    manifest.mutable_blocks(0)->set_address(0x1020);
    EXPECT_THROW(plan(), std::invalid_argument);
    manifest = original;
    manifest.mutable_blocks(0)->set_region_address(std::numeric_limits<uint64_t>::max() - 15);
    EXPECT_THROW(plan(), std::invalid_argument);
    manifest = original;
    manifest.mutable_blocks(0)->set_region_id(0);
    EXPECT_THROW(plan(), std::invalid_argument);
}

TEST_F(HixlReadPlanTest, RejectsUnregisteredDestination) {
    memory.registered = false;
    EXPECT_THROW(plan(), std::invalid_argument);
}

TEST_F(HixlReadPlanTest, RejectsAbsentStatusFailureOrMissingEngine) {
    auto original = manifest;
    manifest.clear_status();
    EXPECT_THROW(plan(), std::invalid_argument);
    manifest = original;
    manifest.set_status(hixl_proto::DEADLINE_EXCEEDED);
    EXPECT_THROW(plan(), std::invalid_argument);
    manifest = original;
    manifest.clear_engine_id();
    EXPECT_THROW(plan(), std::invalid_argument);
}

TEST_F(HixlReadPlanTest, RejectsNullAndWrappingDestination) {
    auto block = destination.getBlock("0");
    block->addr.reset();
    EXPECT_THROW(plan(), std::invalid_argument);
    block->addr =
        std::shared_ptr<void>(reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - 7), [](void*) {});
    EXPECT_THROW(plan(), std::invalid_argument);
}

}  // namespace
}  // namespace rtp_llm
