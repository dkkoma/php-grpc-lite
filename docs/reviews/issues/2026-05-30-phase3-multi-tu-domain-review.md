# Phase 3 multi translation unit domain review 2026-05-30

## Scope

- `config.m4`
- `main.c`
- `internal.h`
- `surface.c`
- `transport.c`
- `unary_call.c`
- `server_streaming_call.c`
- `bridge.c`
- `diagnostic.c`
- `bench.c`
- `docs/issues/open/2026-05-29-c-maintainability-work-plan.md`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer

## Review Prompt Summary

- Phase 3の未コミット変更について、production C extensionが `main.c` の unity-build `.c` include から複数translation unitへ移り、`config.m4` が production source と bench-only diagnostic sourceを分けてcompileする境界変更を確認した。connection / stream / call / channel ownership、metadata / status / deadline semantics、RST_STREAM / GOAWAY / EOF lifecycle、production vs bench/diagnostic boundary、public/internal C API exposureをレビューした。

## Issues

- none

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none

## Evidence

- `config.m4` はproduction buildで `main.c protocol_core.c status_core.c transport_core.c surface.c transport.c unary_call.c server_streaming_call.c bridge.c` をcompileし、`--enable-grpc-bench` 時だけ `diagnostic.c bench.c` を追加している。runtime transport selectionやlibcurl fallbackは追加されていない。
- `main.c` は `surface.c` / `transport.c` / `unary_call.c` / `server_streaming_call.c` / `bridge.c` / `diagnostic.c` / `bench.c` のdirect includeを外し、module globals、Zend class entries、object handlers、server streaming resource id、MINIT / MSHUTDOWN / MINFOの所有元に寄っている。
- production buildの `grpc_lite_functions` は `main.c` で空のfunction tableとして定義され、bench buildでは `bench.c` がdiagnostic PHP functionsを定義する。`tests/phpt/001-load.phpt` はproduction buildで `grpc_lite_unary` / `grpc_lite_bench_unary_batch` などが公開されないことを確認している。
- `internal.h` は private implementation header のままで、install用public C APIやPHP userland surfaceを増やしていない。今回extern化された関数は既存translation unit間の内部linkageを明示するためのもので、HTTP/2 transport以外のruntime pathを公開していない。
- `transport.c` の connection cache、stream user data registration、GOAWAY draining、EOF / socket errorのdead marking、RST_STREAMのstream-local status mapping、metadata/status/deadline処理は今回の差分で責務移動していない。
- `unary_call.c` と `server_streaming_call.c` は引き続きcall orchestrationを担当し、socket/TLS/nghttp2 session ownershipは `h2_connection` / transport側に残している。server streaming resourceは既存どおりpull delivery、cancel、queue/backpressure stateを保持する。
- `bridge.c` は official wrapperの `Grpc\Call::startBatch()` と production unary / server streaming helperの間の写像に留まり、connection cache lifecycleを直接所有していない。

## Domain Model Assessment

- Naming: `Channel` はtarget / credentials / authority / channel options、`h2_connection` はsocket / TLS / nghttp2 session、`grpc_call` はmethod / deadline / metadata / status / stream id、`server_streaming_call_state` はpull-based server streaming resourceという既存語彙を維持している。
- Responsibility boundary: Phase 3はlinkageとbuild graphの変更であり、gRPC framing、metadata/status taxonomy、deadline enforcement、HTTP/2 connection lifecycleのdomain ownerを移していない。
- Connection / stream lifecycle: stream registration and unregister remain in transport ownership, and detached persistent connections are still destroyed only after stream owner count reaches zero. GOAWAY marks the connection draining, and unusable persistent connections are removed from cache.
- Metadata / status / deadline semantics: request metadata filtering, library-owned metadata suppression, response metadata/trailer mapping, grpc-status resolution, and deadline application to connect / TLS / send / recv remain on the same production paths.
- Production vs diagnostic boundary: bench-only diagnostic helpers and PHP functions are guarded by `PHP_GRPC_LITE_ENABLE_BENCH` and are compiled only when `--enable-grpc-bench` is used. Production surface does not gain diagnostic entrypoints.
- Internal/public boundary: The broad `internal.h` extern set is acceptable for this phase because it is explicitly private and matches the issue plan's transition step. It should be narrowed by later header splitting, but it does not currently expose a supported external C API or alter PHP compatibility.

## Residual Risks

- `internal.h` still exposes more transport and protocol helpers across translation units than the final domain boundary should need. This is an acknowledged transitional shape in Phase 3; the next header-splitting phase should reduce the surface to module registration, surface, bridge, call orchestration, transport, and diagnostic-specific headers.
- C symbol visibility is broader than the previous `static` unity-build shape. I did not classify this as a finding because the header is private, no install/public C API is introduced, and the stated Phase 3 purpose is to make cross-translation-unit dependencies explicit before narrower headers are introduced.

## Verification

- Review-only inspection of uncommitted diff, adjacent C files, `docs/SPEC.md`, `docs/code-reading-guide.md`, `docs/protocol-model-review-guide.md`, and the Phase 3 work-plan section.
- `git diff --check`: PASS.
- `rg -n '#include ".*\.c"' main.c surface.c bridge.c transport.c unary_call.c server_streaming_call.c diagnostic.c bench.c protocol_core.c status_core.c transport_core.c tests/unit tests/fuzz`: no matches.
- Docker-based test gates were not rerun in this review session. Parent verification reported normal build/load, bench build/load, C unit, static analysis, PHPT 15/15, C coverage lines 76.8% funcs 94.5%, fuzz 100 runs, PHPUnit 30/109.
