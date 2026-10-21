// Copyright (c) 2026 Alibaba Cloud.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "rtp_llm/cpp/cache/BlockPool.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace rtp_llm {

// A tensor keeps the allocation alive, but only allocator references protect
// individual pages against reuse after logical request cancellation.
class TransferBlockLease {
public:
    TransferBlockLease(BlockPoolPtr pool, BlockIndicesType blocks): pool_(std::move(pool)), blocks_(std::move(blocks)) {
        RTP_LLM_CHECK_WITH_INFO(pool_ != nullptr && !blocks_.empty(), "PD transfer lease requires a pool and blocks");
        std::sort(blocks_.begin(), blocks_.end());
        blocks_.erase(std::unique(blocks_.begin(), blocks_.end()), blocks_.end());
        RTP_LLM_CHECK_WITH_INFO(blocks_.front() > 0 && static_cast<size_t>(blocks_.back()) <= pool_->totalBlocksNum(),
                                "PD transfer lease contains invalid physical block IDs");
        pool_->connectorReference(blocks_);
    }

    ~TransferBlockLease() {
        pool_->connectorFree(blocks_);
    }

    TransferBlockLease(const TransferBlockLease&)            = delete;
    TransferBlockLease& operator=(const TransferBlockLease&) = delete;

private:
    BlockPoolPtr     pool_;
    BlockIndicesType blocks_;
};

}  // namespace rtp_llm
