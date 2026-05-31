# HTTP/2 SETTINGS INI domain review 2026-05-26

## Scope

- `ext/grpc/main.c`
- `ext/grpc/internal.h`
- `ext/grpc/transport.c`
- `ext/grpc/transport_core.c`
- `ext/grpc/tests/002-ini.phpt`
- `ext/grpc/tests/029-trace-file.phpt`
- `ext/grpc/tests/unit/test_transport_core.c`
- `docs/guides/code-reading-guide.md`
- `docs/design/http2-transport-design.md`
- `docs/issues/open/2026-05-26-http2-settings-frame-and-header-list-size.md`

## Reviewer Role

- HTTP/2/gRPC domain model reviewer

## Review Prompt Summary

- Review whether adding `grpc_lite.http2_max_frame_size` and `grpc_lite.http2_max_header_list_size` correctly models HTTP/2 SETTINGS semantics, keeps gRPC transport responsibilities separated from ext-grpc wire emulation, preserves production safety, and has adequate tests/docs.

## Issues

No findings.

The change models `SETTINGS_MAX_FRAME_SIZE` and `SETTINGS_MAX_HEADER_LIST_SIZE` as HTTP/2 connection-level SETTINGS, not as gRPC metadata or Spanner-specific behavior. Defaults are explicit, bounded, and independent from ext-grpc-specific experimental wire profiles.

- `MAX_FRAME_SIZE` defaults to the HTTP/2 default minimum `16384`.
- `MAX_HEADER_LIST_SIZE` defaults to `65536`, matching the existing grpc-lite response metadata default.
- `0` is not used as a disable sentinel; it is a value passed through after clamping.
- SETTINGS remain connection-level and are not attributed to an RPC in trace tests.

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
