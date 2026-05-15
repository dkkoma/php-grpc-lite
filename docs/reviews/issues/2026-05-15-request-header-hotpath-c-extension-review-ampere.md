# request header hotpath C extension review 2026-05-15

## Scope

- `ext/grpc/internal.h`
- `ext/grpc/transport.c`
- Call-site impact in `ext/grpc/unary_call.c`, `ext/grpc/server_streaming_call.c`, `ext/grpc/bench.c`
- PHPT relevance around `ext/grpc/tests/020-request-metadata-control.phpt` and metadata compatibility coverage

## Reviewer Role

- C/PHP extension safety reviewer

## Review Prompt Summary

- Review current working tree request header inline/grow changes for memory ownership, `efree` / release symmetry, allocation failure behavior, uninitialized reads, overflow/capacity invariants, PHPT coverage relevance, and metadata behavior regression risk. Do not modify production code.

## Issues

### REVIEW-20260515-001: inline capacity growth success path lacks PHPT boundary assertion

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `C/PHP extension safety reviewer`
- Finding: Existing PHPT coverage exercises request metadata validation and the `257` value count failure path, but does not directly assert a successful request whose custom metadata count crosses `GRPC_LITE_REQUEST_HEADERS_INLINE_CAPACITY` and still round-trips all values in order.
- Evidence: `ext/grpc/tests/020-request-metadata-control.phpt` covers small successful metadata cases and `257` too-many-values failures for unary/server streaming; the reviewed change moves from pre-counted heap allocation to inline `16` slots plus `grow_request_headers()` in `ext/grpc/transport.c`.
- Expected model: The request header builder should preserve existing metadata semantics across inline and heap-backed storage, especially duplicate/list values and binary metadata, and tests should cover the storage-mode boundary introduced by the optimization.
- Why it matters: The code review did not find an ownership or capacity defect, but without a success case around `17+` custom values, a future regression in grow/copy ordering, `custom_value_count`, or `nghttp2_nv` pointer preservation could pass the current PHPT gate while changing metadata behavior.
- Recommended fix: Add or extend PHPT coverage with unary and preferably server streaming metadata containing more than `16` total request headers/custom values, asserting all echoed values survive in order; include a boundary case near `256` successful custom values if test runtime remains acceptable.
- Fix summary: `ext/grpc/tests/020-request-metadata-control.phpt` に24 valueのcustom metadataをunary/server streaming双方でround-tripする境界テストを追加した。必須header込みでinline capacityを超え、heap grow後も順序と値が保持されることを検証する。
- Fix commit: `this commit`
- Verification: `./tools/test/check-phpt.sh ext/grpc/tests/020-request-metadata-control.phpt`。preflight/default含め15 tests PASS。
- Notes: This is a coverage gap, not a confirmed production-code bug.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## No-Issue Safety Notes

- Memory ownership: `grow_request_headers()` copies only pointer arrays and frees only heap-backed arrays; `zend_string` ownership remains with `name_strings` / `value_strings` and is released once by `free_request_headers()`.
- Inline cleanup: `free_request_headers()` skips `efree()` for inline arrays and still releases tracked `zend_string` entries by `name_count` / `value_count`, so inline and heap paths are symmetric.
- Capacity invariants: fixed headers plus optional `grpc-timeout` reserve `7` slots, custom metadata is capped by `custom_value_count <= GRPC_LITE_MAX_REQUEST_METADATA_VALUES`, and heap capacity caps at `GRPC_LITE_MAX_REQUEST_METADATA_VALUES + 7`.
- Uninitialized reads: `nghttp2_submit_request()` consumes only `headers->len` entries, while cleanup loops consume only `name_count` / `value_count`; unused inline slots are not read.
- Metadata behavior: validation/filtering logic for reserved keys, binary encoding, ASCII validation, duplicate/list values, and insertion order appears unchanged by the inline/grow storage change.
