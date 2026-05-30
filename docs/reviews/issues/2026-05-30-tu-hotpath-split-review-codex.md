# TU hotpath split review 2026-05-30

## Scope

- `config.m4`
- `src/transport.c` / `src/transport.h`
- `src/wrapper_adapter.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `src/status_core.c` / `src/status_core.h`
- `src/protocol_core.c` / `src/protocol_core.h`
- `src/transport_core.c` / `src/transport_core.h`
- `src/grpc_exchange_state.h`
- `src/grpc_result.h`
- `src/diagnostic/bench_call.h`

## Reviewer Role

- Independent C translation-unit / hot-path boundary reviewer

## Review Prompt Summary

- Review whether the current C source/header split is appropriate for domain responsibility boundaries and non-LTO hot-path performance. Focus on whether clearly hot, tightly coupled code was split across TUs without a strong boundary reason; whether cross-TU helpers should be static/private or static inline; whether headers expose unrelated internals; whether `transport.c` is large for good reasons; and whether bench/diagnostic state remains isolated. Production code was not modified.

## Issues

### REVIEW-20260530-001: `transport_core.c` depends on full transport internals for an authority buffer size

- Severity: `Low`
- Status: `Closed`
- Reviewer role: `Independent C translation-unit / hot-path boundary reviewer`
- Finding: `src/transport_core.c` is documented as "Pure HTTP/2 transport helpers shared by the PHP extension and C unit tests", but it includes `src/transport.h` and reaches into `struct _h2_connection` layout only to obtain the size of `authority`. This makes the pure helper TU depend on the broad transport-private header, including nghttp2 callback/protocol declarations and stream/call ownership types.
- Evidence: `src/transport_core.c:1`, `src/transport_core.c:6`, `src/transport_core.c:167`, `src/transport_core.c:172`, `src/transport.h:23`, `src/transport.h:31`, `src/transport.h:78-149`
- Expected model: A pure core helper TU should depend on its own narrow header and shared constants, not on the concrete HTTP/2 connection struct. The authority buffer capacity is a transport identity/input limit and can be represented as a named constant shared by `h2_connection` and `validate_channel_inputs()`.
- Why it matters: This is not a hot-path performance regression, but it weakens the purpose of the split. Any future change to `transport.h` now rebuilds and conceptually affects the pure helper TU, and the pure helper boundary can gradually become a dumping ground for transport internals. It also makes it harder to reason about which modules are allowed to know the full connection layout.
- Recommended fix: Introduce a private constant such as `GRPC_LITE_AUTHORITY_MAX_BYTES` or `GRPC_LITE_AUTHORITY_BUFFER_SIZE` in `transport_core.h` or another narrow internal header. Use it for `h2_connection.authority` and for `validate_channel_inputs()`, then let `transport_core.c` include `transport_core.h` instead of `transport.h`.
- Fix summary: `GRPC_LITE_AUTHORITY_BUFFER_SIZE` を `src/transport_core.h` に追加し、`h2_connection.authority` と `validate_channel_inputs()` が同じ定数を参照する形に変更した。`src/transport_core.c` は `src/transport.h` ではなく `src/transport_core.h` をincludeするようになり、pure helper TUが `h2_connection` layoutを知る必要をなくした。
- Fix commit: `974766e`
- Verification: 通常build、C unit、PHPT 15/15、C static analysis、`git diff --check` PASS。
- Notes: This is intentionally Low because it does not move obvious per-frame/per-byte hot logic across a TU boundary.

## Review Notes

- I did not find a clear non-LTO hot-path regression from the PR split. The dense per-frame/per-byte path remains co-located in `src/transport.c`: nghttp2 callbacks, request data source reads, response data parsing, metadata accumulation, stream close handling, and connection send/recv are still in the same TU. The cross-TU calls from `src/unary_call.c` and `src/server_streaming_call.c` are orchestration-level calls such as `send_pending_h2_frames()`, `connection_recv()`, `register_grpc_call_stream()`, request header construction helpers, and result/status copying. Those calls sit around socket I/O, nghttp2 work, PHP/zend allocation, or one-per-RPC setup/teardown, so I would not treat them as obvious inline candidates without measurement.
- `src/protocol_core.c` and `src/status_core.c` are acceptable splits from a hot-path perspective. `grpc_protocol_parse_status_value()`, content-type/encoding checks, timeout formatting, and status taxonomy are small and testable domain helpers. They are not called per byte; most are called once per relevant header or once per RPC status resolution. Keeping them testable as core helpers is a stronger boundary reason than forcing them into `transport.c`.
- `src/grpc_exchange_state.h`, `src/grpc_result.h`, and `src/diagnostic/bench_call.h` are an improvement over the former catch-all `call.h`. `grpc_exchange_state.h` owns the cross-module exchange state, `grpc_result.h` owns wrapper/orchestration DTOs, and `diagnostic/bench_call.h` is guarded by `PHP_GRPC_LITE_ENABLE_BENCH`.
- `src/transport.h` is broad, but that appears to be a transitional internal facade rather than a public C API leak. It should not be expanded further. If future cleanup splits it, prioritize narrow headers by responsibility (`transport_connection.h`, `transport_stream.h`, `grpc_metadata.h`, or similar) and keep callback/data parser internals in the same TU unless a benchmark justifies moving them.
- Keeping `src/transport.c` large is defensible right now. It keeps the nghttp2 callback graph, active stream lookup, RST/GOAWAY handling, response frame parsing, metadata/status accumulation, and send/recv deadlines together. Low-risk future splits would be cold/setup-heavy code such as TLS setup, trace-file formatting, or persistent connection key/cache helpers, but those are cleanup candidates rather than findings. I would avoid splitting `on_data_chunk_recv_callback()`, `grpc_protocol_process_response_data_direct()`, `enqueue_response_payload()`, `send_callback()`, `data_source_read_callback()`, and `send_pending_h2_frames()` out of the current TU without before/after measurement.
- Bench/diagnostic state is sufficiently isolated for production builds. `config.m4` only compiles `src/diagnostic/diagnostic.c` and `src/diagnostic/bench.c` when `--enable-grpc-bench` is enabled, and the large `grpc_bench_call` state is included in `grpc_call` only under `PHP_GRPC_LITE_ENABLE_BENCH`.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `1`
- Design Decision: `2`
