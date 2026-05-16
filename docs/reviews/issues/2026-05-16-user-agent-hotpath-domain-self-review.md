---
Status: Closed
Owner: Codex
Created: 2026-05-16
Scope: ext/grpc bridge, unary, server streaming, transport request header path
Role: HTTP/2/gRPC domain model self-review
---

# user-agent hotpath self-review

## Review target

- `ext/grpc/bridge.c`
- `ext/grpc/unary_call.c`
- `ext/grpc/server_streaming_call.c`
- `ext/grpc/transport.c`
- `ext/grpc/internal.h`
- `ext/grpc/bench.c`

## Findings

- Blocker: none
- High: none
- Medium: none
- Low: none

## Checks

- gRPC request metadata filtering keeps user supplied `user-agent` out of custom metadata traversal.
- The wire `user-agent` remains deterministic and is appended by the HTTP/2 request header builder.
- Unary and server streaming native paths use the same header construction rule.
- Franken-go path still receives metadata with `user-agent`, because it is a PHP backend API rather than the native HTTP/2 header builder.
- Diagnostic bench direct APIs pass `NULL` and therefore use the default `php-grpc-lite/0.1.0` header.
- `primary_user_agent` lifetime is owned by `Grpc\Channel`; nghttp2 receives header pointers only during synchronous submit/send while the channel remains alive.

## Verification

- `make test TESTS="tests/020-request-metadata-control.phpt tests/023-metadata-and-call-credentials.phpt tests/026-franken-go-backend.phpt tests/010-unary.phpt tests/011-server-streaming.phpt"`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-c-static-analysis.sh`: PASS
