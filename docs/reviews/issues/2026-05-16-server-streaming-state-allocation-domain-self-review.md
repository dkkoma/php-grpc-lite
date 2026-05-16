# server streaming state allocation cleanup domain review 2026-05-16

## Scope

- `ext/grpc/internal.h`
- `ext/grpc/server_streaming_call.c`
- `ext/grpc/transport.c`
- `docs/issues/closed/2026-05-16-server-streaming-state-allocation-hotpath.md`

## Reviewer Role

- Parent agent domain model reviewer

## Review Prompt Summary

- Review whether removing `server_streaming_call_state.path` and `server_streaming_call_state.metadata` preserves HTTP/2/gRPC stream lifecycle, PHP resource cleanup, and production/diagnostic responsibility boundaries.

## Issues

### REVIEW-20260516-001: none

- Severity: `Low`
- Status: `Rejected`
- Reviewer role: `Parent agent domain model reviewer`
- Finding: `No issue found.`
- Evidence: `server_streaming_call_open_resource()` uses `path` only while building HTTP/2 headers and registering the request; after `nghttp2_submit_request()` there is no retry/re-submit path. `headers_zv` is consumed synchronously by `append_custom_request_headers()` before the stream resource is returned. `destroy_server_streaming_call_state()` only released these fields and no cleanup logic depended on their contents.
- Expected model: `server_streaming_call_state` should own only state needed after the stream resource is returned: the `grpc_call`, request payload backing memory for nghttp2 data provider, receive buffer, delivery counters, and lifecycle flags.
- Why it matters: Holding unused PHP zvals in a long-lived stream resource adds avoidable refcount work and makes the state type imply responsibilities it does not have.
- Recommended fix: `none`
- Fix summary: `Existing change is consistent with the expected model.`
- Fix commit: `pending`
- Verification: `Target PHPT, full PHPT, static analysis, and real Spanner mixed c16 benchmark recorded in docs/issues/closed/2026-05-16-server-streaming-state-allocation-hotpath.md.`
- Notes: `Request payload ownership remains via state->request because nghttp2 data provider can read it after submit; that field is correctly retained.`

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
