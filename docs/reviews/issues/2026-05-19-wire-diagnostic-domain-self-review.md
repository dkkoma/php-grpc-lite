# Wire diagnostic domain self review 2026-05-19

## Scope

- `ext/grpc/transport.c`
- `ext/grpc/unary_call.c`
- `ext/grpc/server_streaming_call.c`
- `ext/grpc/internal.h`
- `ext/grpc/tests/029-trace-file.phpt`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer

## Review Prompt Summary

- Review whether opt-in wire diagnostic correctly models gRPC request headers, HTTP/2 frame boundaries, call lifecycle ownership, and production vs diagnostic boundary.

## Issues

### REVIEW-20260519-001: Wire diagnostic is diagnostic-only but compiled in production

- Severity: Design Decision
- Status: Accepted
- Reviewer role: HTTP/2 / gRPC domain model reviewer
- Finding: `GRPC_LITE_TRACE_FILE` is an opt-in runtime diagnostic path compiled into the production extension.
- Evidence: `ext/grpc/transport.c`, `grpc_lite_trace_file_path`, `grpc_lite_trace_request_headers`, `grpc_lite_trace_outbound_frame`.
- Expected model: Production behavior must not change unless diagnostic env is explicitly enabled.
- Why it matters: The investigation needs reporter-side observability, but normal users must not pay file I/O or leak wire details by default.
- Recommended fix: Keep all trace paths guarded by `GRPC_LITE_TRACE_FILE`; avoid public API surface; redact sensitive header values; test event presence only when env is set.
- Fix summary: Trace functions return immediately without `GRPC_LITE_TRACE_FILE`; sensitive headers are value-redacted; PHPT covers opt-in behavior.
- Fix commit: pending
- Verification: `./tools/test/check-phpt.sh`, `./tools/test/check-c-static-analysis.sh`, real Spanner smoke.
- Notes: `GRPC_LITE_TRACE_WIRE_BYTES=1` can emit HPACK bytes and must not be pasted into public issues.

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: 1 accepted
