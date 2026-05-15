# Channel key cache domain model review 2026-05-16

## Scope

- `ext/grpc/internal.h`
- `ext/grpc/surface.c`
- `ext/grpc/bridge.c`
- `docs/issues/open/2026-05-16-cache-channel-key-and-credential-identity.md`

## Reviewer Role

- HTTP/2/gRPC domain model self reviewer

## Review Prompt Summary

- Review whether caching the HTTP/2 persistent connection key on `Grpc\Channel` correctly models channel identity, credential identity, connection lifecycle, and public/internal boundaries without changing gRPC semantics.

## Issues

### REVIEW-20260516-001: none

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `HTTP/2/gRPC domain model self reviewer`
- Finding: `No Blocker/High/Medium/Low findings. Caching connection identity on Channel is the correct owner boundary because target, authority, TLS override, credentials, and metadata size options are immutable after Channel construction in this implementation.`
- Evidence: `ext/grpc/surface.c:Channel::__construct`, `ext/grpc/bridge.c:grpc_lite_perform_call_unary`, `ext/grpc/bridge.c:grpc_lite_open_call_stream`, `ext/grpc/surface.c:Channel::close`
- Expected model: `gRPC Channel owns stable connection identity. RPC Call should use the Channel identity and must not recompute credential PEM identity on every RPC.`
- Why it matters: `Connection reuse key must remain stable across RPCs and Channel::close, while avoiding hot path work that belongs to Channel construction.`
- Recommended fix: `Keep cached key as internal Channel state; ensure cleanup releases it and all call paths use it read-only.`
- Fix summary: `Implemented in current working tree.`
- Fix commit: `pending`
- Verification: `PHPT lifecycle/unary/server-streaming/TLS passed; real Spanner mixed c16 benchmark showed CPU/request improvement.`
- Notes: `The cached key remains internal and does not expose persistent connection details through public API.`

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `1 accepted`
