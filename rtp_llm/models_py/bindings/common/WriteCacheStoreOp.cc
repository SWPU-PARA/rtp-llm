#include "rtp_llm/models_py/bindings/common/WriteCacheStoreOp.h"
#include "rtp_llm/models_py/bindings/core/ExecOps.h"
#include "rtp_llm/models_py/bindings/core/CacheStoreAsyncWriter.h"

namespace rtp_llm {

using namespace torch_ext;

void WriteCacheStoreOp(const torch::Tensor&                         input_lengths,
                       const torch::Tensor&                         prefix_lengths,
                       const torch::Tensor&                         kv_cache_block_id_host,
                       std::optional<torch_ext::PyCacheStoreInputs> cache_store_member,
                       std::optional<torch_ext::LayerKVCache>       kv_cache) {
    if (!kv_cache.has_value() || !cache_store_member.has_value()) {
        return;
    }

    const auto& cache_store_inputs = cache_store_member.value();

    // Retain physical pages on the caller thread. Tensor references alone do
    // not protect pages while the asynchronous writer is waiting in its queue.
    CacheStoreInputs inputs{input_lengths,
                            prefix_lengths,
                            kv_cache_block_id_host,
                            cache_store_inputs.kv_cache_layer_to_group,
                            cache_store_inputs.kv_cache_group_types,
                            cache_store_inputs.context_batch_size,
                            cache_store_inputs.decoder_batch_size,
                            cache_store_inputs.request_id,
                            cache_store_inputs.request_pd_separation,
                            cache_store_inputs.cache_keys,
                            cache_store_inputs.tokens_per_block,
                            cache_store_inputs.kv_block_stride_bytes,
                            cache_store_inputs.kv_scale_stride_bytes,
                            cache_store_inputs.pd_separation,
                            cache_store_inputs.model_id,
                            cache_store_inputs.decode_entrance,
                            cache_store_inputs.warmup,
                            kv_cache->layer_id,
                            runtimeCreateEvent(),
                            {}};
    inputs.publication_leases = retainCacheStoreBlocks(inputs, cache_store_inputs.cache_store);

    auto run = [inputs               = std::move(inputs),
                captured_cache_store = cache_store_inputs,
                captured_kv_cache    = kv_cache.value()]() mutable {
        KvCacheInfo kv_cache_info;
        kv_cache_info.kv_cache_buffer = captured_kv_cache.kv_cache_base;
        kv_cache_info.kv_scale_buffer =
            (captured_kv_cache.kv_scale_base.defined() && captured_kv_cache.kv_scale_base.numel() > 0) ?
                captured_kv_cache.kv_scale_base :
                torch::Tensor();
        execWriteCacheStore(inputs, kv_cache_info, captured_cache_store.mla_kvcache, captured_cache_store.cache_store);
    };

    auto* async_writer = cache_store_inputs.cache_store_async_writer;
    if (async_writer) {
        async_writer->submit(std::move(run));
    } else {
        run();
    }
}

}  // namespace rtp_llm
