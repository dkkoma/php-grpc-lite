# Default backend HTTP/2 domain review 2026-05-27

## Scope

- `ext/grpc/main.c`
- `ext/grpc/tests/002-ini.phpt`
- `README.md`
- `docs/http2-transport-design.md`
- `docs/frankenphp-go-backend-design.md`
- `docs/frankenphp-grpc-go-client-change-request.md`
- `docs/issues/open/2026-05-27-default-backend-http2.md`

## Reviewer Role

- gRPC transport backend selection domain reviewer

## Review Prompt Summary

- Review whether changing `grpc_lite.backend` default from `auto` to `http2` correctly models php-grpc-lite's production default transport, keeps the optional FrankenPHP grpc-go backend explicit, and preserves compatibility for explicit `franken-go` / `auto` usage.

## Issues

No findings.

The change aligns the production default with the repository model: the nghttp2 HTTP/2 backend is the normal runtime transport, and FrankenPHP grpc-go is an optional backend selected explicitly. Keeping `auto` as an accepted explicit value preserves test/compatibility usage without making class-presence based backend selection the default.

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
