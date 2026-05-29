# Phase 5 src layout domain review 2026-05-30

## Scope

- `config.m4`
- `grpc.c`
- `src/`
- `src/diagnostic/`
- `docs/code-reading-guide.md`
- `docs/protocol-model-review-guide.md`
- Phase 5 layout move plus PR follow-up: C source/internal headers under `src/`, diagnostic bench files under `src/diagnostic`, repository root retaining `config.m4`, `php_grpc.h`, and `grpc.c`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer

## Review Prompt Summary

- Phase 5のsrc layout移動について、HTTP/2/gRPC domain modelだけを対象に、命名、責務、connection / stream / call / channel scope、flow-control、metadata/status/deadline、RST_STREAM / GOAWAY / EOF lifecycle、production / bench boundaryが移動後も崩れていないかを確認した。

## Issues

### REVIEW-20260530-001: `src/diagnostic/bench.c` のownership commentが旧includeモデルを指している

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `src/diagnostic/bench.c` の先頭コメントが「This file is included from main.c intentionally」と説明しているが、Phase 5後の実際の所有モデルでは `bench.c` は `main.c` にincludeされず、`config.m4` が `--enable-grpc-bench` 時だけ `src/diagnostic/diagnostic.c` と `src/diagnostic/bench.c` をextension source listへ追加している。
- Evidence: the old `src/diagnostic/bench.c` comment said the file was included from `main.c`; `grpc.c` only defines the empty production `grpc_lite_functions` table when bench is disabled; `config.m4` owns the bench source inclusion boundary; `src/diagnostic/bench.c` defines the bench function table for bench builds.
- Expected model: Diagnostic / benchmark entrypoints should be documented as a separate bench-only compilation unit selected by build configuration. Production `grpc.c` remains the module root for class/INI/module lifecycle and must not appear to own or include diagnostic transport entrypoint implementation.
- Why it matters: This does not change runtime protocol behavior, but it leaves a stale production-vs-diagnostic boundary description exactly at the point where Phase 5 is trying to make that boundary visible through layout. Future maintainers could incorrectly preserve an old include-based ABI assumption or reintroduce `main.c` coupling when touching bench-only HTTP/2 helpers.
- Recommended fix: Update the comment to describe the current model: `src/diagnostic/bench.c` is compiled only by `config.m4` when `PHP_GRPC_LITE_ENABLE_BENCH` is enabled, and it intentionally keeps bench entrypoints out of the production source list while reusing private transport/call helpers.
- Fix summary: `src/diagnostic/bench.c` の先頭コメントを、`main.c` includeモデルではなく `config.m4` による `--enable-grpc-bench` 限定コンパイルモデルの説明へ更新した。
- Fix commit: `1642c64`
- Verification: `git diff --check`: PASS. Phase 5 gates are recorded in `docs/issues/open/2026-05-29-c-maintainability-work-plan.md`.
- Notes: This is a documentation/domain-boundary finding only. The actual build boundary still appears correct.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## Evidence

- `config.m4:15` keeps the production source list to `grpc.c` plus `src/protocol_core.c`, `src/status_core.c`, `src/transport_core.c`, `src/surface.c`, `src/transport.c`, `src/unary_call.c`, `src/server_streaming_call.c`, and `src/bridge.c`.
- `config.m4:16`-`config.m4:17` adds `src/diagnostic/diagnostic.c` and `src/diagnostic/bench.c` only when `PHP_GRPC_BENCH` is enabled.
- `grpc.c` includes `php_grpc.h`, `src/surface.h`, and `src/transport.h`; it does not include moved `.c` files.
- `grpc.c` keeps the production PHP function table empty when bench entrypoints are not enabled.
- `src/diagnostic/bench.c:2235`-`src/diagnostic/bench.c:2242` owns the diagnostic PHP function table for bench builds.
- `src/surface.h:18`-`src/surface.h:35` keeps gRPC Channel identity/options in the PHP `Grpc\Channel` object and does not add socket, TLS, nghttp2, response body, or stream progress state.
- `src/call.h:175` onward keeps gRPC call / HTTP/2 stream-local state such as method path, stream id, status, metadata, deadline, response parse state, and request write state in `grpc_call`.
- `src/transport.h:22` onward keeps HTTP/2 connection state such as fd, TLS, nghttp2 session, dead/draining state, GOAWAY details, active stream table, and current I/O bookkeeping in `h2_connection`.
- `src/transport.h:55` onward keeps server streaming resource state as a wrapper around one `grpc_call`, request buffer, delivery counters, and completion/cancel flags; it does not take over channel identity or unrelated stream state.
- `src/unary_call.c:1` and `src/server_streaming_call.c:1` still name the call-type orchestration layer explicitly as unary and server streaming execution over an HTTP/2 connection.
- `src/bridge.h:4` includes `src/diagnostic/diagnostic.h`, and `src/diagnostic/diagnostic.h:7`-`src/diagnostic/diagnostic.h:10` guards diagnostic declarations with `PHP_GRPC_LITE_ENABLE_BENCH`. This keeps symbols out of production builds, though the include dependency remains a residual boundary smell inherited from the pre-move header shape.

## Domain Model Assessment

- Naming: Moving implementation files under `src/` and diagnostic files under `src/diagnostic/` improves the visible distinction between production extension internals and bench-only entrypoints. The existing `surface`, `call`, `transport`, `unary_call`, `server_streaming_call`, and `diagnostic` names still map to the repository's documented domain objects.
- Responsibility boundary: `grpc.c` remains the module root for globals, INI, PHP class registration, and module lifecycle. `php_grpc.h` carries the module declaration and version macro. `src/surface.c` / `src/surface.h` remain PHP object surface, `src/bridge.c` remains official-wrapper batch mapping, `src/unary_call.c` and `src/server_streaming_call.c` remain call orchestration, and `src/transport.c` / `src/transport.h` remain HTTP/2 transport ownership.
- Connection / stream / call / channel scope: The move does not collapse channel identity into `h2_connection`, move socket ownership into PHP call objects, or move per-call status/metadata into the persistent cache. `h2_connection`, `grpc_call`, `grpc_lite_channel_obj`, and `server_streaming_call_state` remain separate domain objects.
- Flow-control: The layout move does not change the ownership of stream/connection window settings, WINDOW_UPDATE accounting, or receive queue limits. Flow-control state remains in the transport/call/server-streaming resource boundary rather than in the PHP surface.
- Metadata / status / deadline: Request metadata, library-owned metadata, grpc-timeout/deadline conversion, response initial/trailing metadata, grpc-status resolution, and size-limit status mapping remain in the same call/transport helpers; the move only changes paths.
- RST_STREAM / GOAWAY / EOF lifecycle: Stream-local state (`stream_reset_seen`, `stream_error_code`, `stream_closed`) remains on `grpc_call`; connection lifecycle state (`dead`, `draining`, `last_goaway_*`) remains on `h2_connection`. The layout move does not add an implicit retry path or make GOAWAY/draining connection reuse appear valid.
- Production / bench boundary: Build selection is still explicit in `config.m4`, and diagnostic PHP entrypoints remain absent from the production function table. The one Low finding is the stale comment that describes the old include-based ownership model.

## Residual Risks

- Production `.c` files still include `src/diagnostic/diagnostic.h` for bench-only helper declarations, and `src/bridge.h` points into `src/diagnostic/`. The declarations are guarded and do not expose production symbols, so this review does not classify it as a behavioral finding, but the dependency direction remains worth simplifying in a future header cleanup.
- This was a review-only pass. I did not rerun Docker-based build, PHPT, C unit, static analysis, coverage, or PHPUnit gates.

## Verification

- Reviewed the Phase 5 diff, moved source/header layout, `config.m4`, `grpc.c`, `php_grpc.h`, `src/diagnostic/bench.c`, `src/diagnostic/diagnostic.h`, representative production headers, `docs/SPEC.md`, `docs/code-reading-guide.md`, `docs/protocol-model-review-guide.md`, and prior review issue style.
- Original review pass did not edit implementation files. PR follow-up later renamed the module entrypoint source to `grpc.c` and added `php_grpc.h`; this review record was updated to reflect the final PR layout.
