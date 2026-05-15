# Request Header Hotpath Domain Review 2026-05-15

## Scope

- `ext/grpc/internal.h`
- `ext/grpc/transport.c`
- Request header builder call sites in `ext/grpc/unary_call.c`, `ext/grpc/server_streaming_call.c`, and `ext/grpc/bench.c`

## Reviewer Role

- HTTP/2/gRPC protocol domain model reviewer

## Review Prompt Summary

- Review current working tree request header building changes from the repository protocol/domain-model perspective, covering HTTP/2/gRPC metadata semantics, request header lifecycle, memory ownership, capacity invariants, production/bench boundaries, and compatibility.

## Issues

- No issues found.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## Notes

- The one-pass request metadata builder preserves the gRPC request model: fixed pseudo/protocol headers are appended before user metadata, reserved/forbidden user metadata remains rejected, binary metadata is still base64-encoded before submission, and the `GRPC_LITE_MAX_REQUEST_METADATA_VALUES` limit continues to apply to custom metadata values rather than fixed transport headers.
- `h2_request_headers` ownership remains explicit: inline buffers are owned by the stack struct, grown heap buffers are freed only when they replace inline buffers, and retained `zend_string` names/values are released through `free_request_headers()`.
- Capacity growth maintains the required total header capacity of fixed headers plus custom metadata values, including optional `grpc-timeout`, without moving benchmark-only behavior into production semantics.
- The helper is still shared by production and bench paths, but the change does not add benchmark-specific state or policy to the production request header lifecycle.
