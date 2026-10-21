# Ascend HIXL PD transport

This adapter adds device-to-device cache reads to the existing Prefill/Decode
separation flow. It requires Ascend hardware with HIXL support and CANN 9.2.0.
The scope is TP1 Prefill and TP1 Decode, with optional MTP. MTP reuses the
existing draft-cache transfer and speculative-decoding protocol. Prefill copies
proposal tensors to CPU before RPC serialization; Decode retains draft-cache
destination pages through the same block ownership used by the main cache.

## Data flow

1. Decode allocates destination cache blocks using the existing allocator.
2. Prefill publishes each layer through the existing asynchronous cache writer,
   after the layer completion event. Existing block keys and group mappings apply.
3. Decode requests block addresses and registered ranges over the existing ARPC
   infrastructure. The HIXL control service returns a manifest and a source grant.
4. Decode validates the manifest and issues asynchronous HIXL READ operations
   directly into its registered device cache. Each descriptor covers one block.
5. Physical READ completion triggers the original load callback. Decode then
   retires the source grant through an idempotent control RPC.

Physical page leases use the existing BlockPool connector references. Prefill
acquires leases before enqueueing the writer; Decode retains its destination
through READ completion or physical disconnect. Request cancellation and logical
timeouts do not release memory that may still be in DMA use.

The existing `BlockBuffer::addr` shares ownership of these page references;
there is no separate ownership field. Once Decode confirms loading, Prefill
ends cache publication at the original handoff point. Pending grants still
retain their pages; token forwarding continues through the existing RPC stream.

## Code ownership

`cache_store/Impl.cpp` selects the transport through the existing factories.
Shared callers use `MemoryUtil::requiresBlockLease()` to retain allocator pages;
the default is false. HIXL-specific state stays in `cache_store/ascend/`:

- `HixlMessager` connects the existing load API to the control and data paths.
- `HixlManifestService` publishes registered addresses through the existing ARPC server.
  `HixlManifest.h/.cc` groups source-grant ownership with manifest validation.
- `HixlReadWorker.h/.cc` contains `HixlLoadScheduler` and `HixlReadProgress`.
  Admission lasts through source retirement; physical DMA completion or disconnect
  is tracked separately. These workers depend only on the standard library.
- `HixlMemoryUtil` owns registrations. The SDK read driver is private to
  `HixlMessager.cc`; `HixlReadDriver` keeps progress tests independent of the SDK.

Retirement uses the reader instance, engine and transfer sequence. It is
idempotent and may precede publication when the manifest response is lost.
Deployment requires trusted workers and a trusted network.

Deploy the same adapter revision on both workers. Earlier readers required the
removed manifest grant ID; drain and restart both workers when upgrading.

## Build and tests

Use the repository's Ascend Bazel configuration with CANN 9.2.0.
The product target is `//rtp_llm/cpp/disaggregate/cache_store/ascend:hixl_messager`.
Tests live in `ascend/test/BUILD` and use the repository's `cc_test_wrapper`.
Run host regressions with `bazel test //rtp_llm/cpp/disaggregate/cache_store/ascend/test:all --test_tag_filters=-manual`.
The retained regressions cover manifest validation, admission, and page lifetime.
Real device transport validation is separate from these host tests.

## Configuration

Keep the existing PD role, model and routing arguments. Add these on both workers:

```sh
--cache_store_transport=hixl
```

The engine address uses the existing CacheStore bind IP and
`cache_store_rdma_listen_port`, normally `start_port + rank_id * 8 + 4`.
This port must be reachable and distinct from the cache-store ARPC port.

LocalCommRes defaults to `{"version":"1.3"}` for Ascend 950 UB/UBC super-plane
topology discovery. For other network configurations, set the complete
per-worker topology JSON through `ASCEND_LOCAL_COMM_RES`:

```sh
export ASCEND_LOCAL_COMM_RES='<HIXL LocalCommRes JSON for this worker>'
```

The adapter passes this JSON to HIXL without rewriting it. The topology must
match the deployment. Automatic discovery still requires device validation
with the installed CANN 9.2.0 SDK.

Internal defaults admit 64 outstanding loads, including manifest and retirement
RPCs, and reserve 8 SDK communication channels (16 streams). These independent
limits protect model resources; they are not server arguments.

The previous four `--hixl_*` arguments and their `HIXL_*` environment bindings
are removed. Replace a custom `HIXL_LOCAL_COMM_RES` with `ASCEND_LOCAL_COMM_RES`
and recreate worker configuration on restart; old 25-field HIXL pickles are
not accepted. The original 20-field baseline configuration remains readable.

An empty `cache_store_transport` preserves the previous transport selection.
HIXL does not silently fall back to TCP after a transfer failure. It supports the
PD pull flow, not remote-store push. TP greater than one and Eagle are rejected.
A failed connection stays closed for this process; restart the affected worker
after resolving the failure. Shutdown terminates the process if physical drain
or grant retirement cannot be confirmed, rather than recycling live DMA pages.
