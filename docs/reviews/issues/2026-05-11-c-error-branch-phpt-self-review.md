# C error branch PHPT self review 2026-05-11

## Scope

- `ext/grpc/tests/020-request-metadata-control.phpt`
- `ext/grpc/tests/024-control-semantics.phpt`
- `ext/grpc/unary_call.c`
- `ext/grpc/server_streaming_call.c`
- `ext/grpc/transport.c`

## Reviewer Role

- Domain model / error semantics self reviewer

## Review Prompt Summary

- Review whether newly added PHPT cases for easy C error branch coverage correctly model gRPC metadata validation, server streaming setup failure, EOF lifecycle, and cancel idempotence without introducing test-only or ambiguous API semantics.

## Issues

### REVIEW-20260511-001: none

- Severity: `none`
- Status: `Closed`
- Reviewer role: `Domain model / error semantics self reviewer`
- Finding: `none`
- Evidence: `ext/grpc/tests/020-request-metadata-control.phpt`, `ext/grpc/tests/024-control-semantics.phpt`
- Expected model: Request metadata validation is API misuse and should raise before transport I/O; server streaming connection setup failure should surface as call status without yielding messages; raw EOF fixture should assert recovery without depending on global fixture parity; cancel after cleanup should be idempotent.
- Why it matters: These cases exercise real public semantics around metadata, connection setup, connection lifecycle, and stream cancellation rather than artificial C-only failures.
- Recommended fix: `none`
- Fix summary: `none`
- Fix commit: `this commit`
- Verification: target PHPT, `./tools/test/check-phpt.sh`, `./tools/test/check-c-coverage.sh`; C total 75.6% → 76.8%, `server_streaming_call.c` 59.5% → 63.8%, `unary_call.c` 73.8% → 75.9%.
- Notes: A scalar metadata success case was considered but removed because the stable public metadata model is array-of-values; coverage should not freeze ambiguous input shape as supported behavior.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
