# duplicate request metadata domain review 2026-05-19

## Scope

- `ext/grpc/bridge.c`
- `ext/grpc/transport.c`
- `ext/grpc/tests/023-metadata-and-call-credentials.phpt`
- `docs/issues/open/2026-05-19-duplicate-request-metadata-preservation.md`
- `docs/issues/open/2026-05-18-github-issue-5-tls-headers-data-write-attribution.md`

## Reviewer Role

- HTTP/2 / gRPC metadata domain model reviewer

## Review Prompt Summary

- Review the current uncommitted duplicate request metadata change, focusing on duplicate request metadata values, CallCredentials plugin merge responsibility, reserved library-owned metadata, compression / `grpc-accept-encoding` boundary, zval ownership/lifecycle where it affects metadata invariants, and production vs diagnostic measurement boundary.

## Issues

- none

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## Accepted Checks

- Duplicate request metadata values are modeled as one PHP metadata key whose value is a list of values, and native request header emission already expands list values into repeated HTTP/2 metadata headers.
- `grpc_lite_merge_call_credentials_metadata()` owns only the CallCredentials plugin merge step: it appends plugin-returned values to existing request metadata without overwriting or dropping same-key user metadata.
- Reserved library-owned metadata remains enforced at the native transport boundary by `append_custom_request_headers()`, including `grpc-*`, `grpc-accept-encoding`, `grpc-encoding`, `grpc-status`, `grpc-message`, pseudo headers, and connection-specific HTTP/2 headers.
- Compression boundary is preserved: the experimental `grpc-accept-encoding` measurement is recorded in the issue notes but is not part of the production change, so php-grpc-lite does not advertise unsupported response compression by default.
- zval ownership preserves the metadata invariant: top-level call metadata is separated before merge, existing scalar values are converted into arrays with copied zvals, existing arrays are separated before append, and plugin return values are copied before the callback return array is destroyed.
- Production vs diagnostic boundary is clean for this change: production behavior is limited to request metadata merge semantics, while the real Spanner measurement notes remain in `docs/issues/` and no diagnostic/measurement hook is introduced into the runtime path.
