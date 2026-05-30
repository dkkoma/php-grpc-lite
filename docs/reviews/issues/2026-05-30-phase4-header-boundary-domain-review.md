# Phase 4 header boundary domain review 2026-05-30

## Scope

- `internal.h`
- `common.h`
- `module.h`
- `surface.h`
- `call.h`
- `transport.h`
- `unary_call.h`
- `server_streaming_call.h`
- `diagnostic.h`
- `bridge.h`
- `main.c`
- `surface.c`
- `transport.c`
- `unary_call.c`
- `server_streaming_call.c`
- `bridge.c`
- `diagnostic.c`
- `bench.c`
- `status_core.c`
- `transport_core.c`
- `tests/unit/test_status_core.c`
- `tests/unit/test_transport_core.c`
- `docs/issues/open/2026-05-29-c-maintainability-work-plan.md`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer

## Review Prompt Summary

- Phase 4の未コミット変更について、`internal.h` を薄いprivate aggregateにし、宣言を責務別internal headerへ移した差分を確認した。connection / stream / call / channel ownership、metadata / status / deadline semantics、RST_STREAM / GOAWAY / EOF lifecycle、production vs bench/diagnostic boundary、public/internal C API exposureをレビューした。

## Issues

- none

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none

## Evidence

- `internal.h:5` は明示的にbackward-compatible private aggregate headerとされ、`internal.h:10` で `bridge.h` を読むだけになっている。install用public C APIやPHP userland surfaceは追加されていない。
- `common.h:13`-`common.h:44` は従来 `internal.h` にあった共有includeと定数の移動先であり、runtime transport choiceやlibcurl fallbackを追加していない。
- `module.h:4` はmodule globalsを `common.h` の上に置き、`main.c:8` の `ZEND_DECLARE_MODULE_GLOBALS(grpc_lite)`、`main.c:38` の GINIT、`main.c:54` の GSHUTDOWN が引き続きmodule lifecycleの所有元になっている。
- `surface.h:25`-`surface.h:57` は `Grpc\Channel` と `Grpc\Call` のPHP object stateを保持し、`call.h:175` 以降の `grpc_call` や `transport.h:22` 以降の `h2_connection` と分かれている。Channel objectがsocket / TLS / nghttp2 sessionを所有する形には変わっていない。
- `call.h:175` 以降の `grpc_call` はmethod path、stream id、status、metadata、deadline、response parse stateを持つcall/stream-local stateで、`transport.h:22` 以降の `h2_connection` はfd、TLS、nghttp2 session、GOAWAY/dead/draining、active stream tableを持つconnection stateとして残っている。
- `transport.h:62` 以降の `server_streaming_call_state` は `grpc_call`、request buffer、delivery counters、completed/cancelled stateを持つserver streaming resource stateであり、connection identityやunrelated stream stateを新たに所有していない。
- `unary_call.h:7` と `server_streaming_call.h:7` はcall orchestration entrypointを分け、unary / server streamingの差分をHTTP/2 transportではなくcall type側のAPI境界として表現している。
- `diagnostic.h:7`-`diagnostic.h:10` のdiagnostic declarationsは `PHP_GRPC_LITE_ENABLE_BENCH` でguardされており、`config.m4:15`-`config.m4:17` はproduction source listに `diagnostic.c` / `bench.c` を含めず、bench build時だけ追加している。
- `main.c:5`-`main.c:6`、`surface.c:3`-`surface.c:4`、`transport.c:1`、`bridge.c:3`、`unary_call.c:3`、`server_streaming_call.c:3`、`status_core.c:6`、`transport_core.c:6`、`tests/unit/test_status_core.c:4`、`tests/unit/test_transport_core.c:6` は `internal.h` ではなく責務別headerを直接includeしている。
- `git diff --check`: PASS。
- `rg -n '#include "[^"]+\.c"' main.c surface.c bridge.c transport.c unary_call.c server_streaming_call.c diagnostic.c bench.c protocol_core.c status_core.c transport_core.c tests/unit tests/fuzz`: no matches。

## Domain Model Assessment

- Naming: `surface.h` はPHP `Grpc\*` object surface、`call.h` はgRPC call / status / metadata / response result state、`transport.h` はHTTP/2 connection / stream registration / transport helpers、`unary_call.h` と `server_streaming_call.h` はcall orchestration、`diagnostic.h` はbench diagnostic declarationsとして分類されている。既存のchannel / connection / stream / call vocabularyと矛盾しない。
- Responsibility boundary: 今回はdeclaration placementとinclude graphの変更であり、gRPC framing、metadata filtering、status taxonomy、deadline enforcement、TLS/mTLS setup、persistent connection lifecycleの実装所有元を移していない。
- Connection / stream lifecycle: `h2_connection` のdead/draining/GOAWAY state、active stream tracking、detached persistent connection destructionの宣言は `transport.h` にまとまり、`grpc_call` のstream-local status / RST_STREAM stateとは分離されたまま。
- Metadata / status / deadline semantics: request headers、grpc-timeout、response metadata/trailer map、grpc-status resolution、message and metadata size limitsはheader move後もtransport/call boundary内の同じ declarations を使う。user metadataやlibrary-owned metadata semanticsを変える差分はない。
- Production vs diagnostic boundary: `--enable-grpc-bench` guardと `config.m4` source listにより、bench diagnostic entrypointsはproduction buildに追加されない。production `Grpc\Call::startBatch()` pathは引き続きbridgeからunary/server streaming orchestrationへ委譲される。
- Internal/public boundary: 追加headerはrepository rootのprivate implementation headerであり、install用 `include/` や外部supported C APIは追加されていない。`internal.h` の互換aggregateはprivateと明記されている。

## Residual Risks

- `diagnostic.h` はbench diagnostic declarationsのために `server_streaming_call.h` と `unary_call.h` をまとめて読む形で、現時点ではprivate header内の依存集約に留まる。production buildでdiagnostic symbolは公開されず、今回のPhase 4スコープではfindingにはしない。将来 `src/diagnostic/` へ移す段階では、production `.c` がdiagnostic headerを直接includeしない形までさらに狭める余地がある。
- `transport.h` はまだHTTP/2 connection lifecycle、request header assembly、nghttp2 callbacks、response metadata/status helpersをまとめて公開している。これは今回の「internal.h から責務別headerへ移す」段階では改善されているが、後続のtransport細分化ではconnection / callbacks / request headers / response processingのprivate headerへさらに分ける余地がある。

## Verification

- Review-only inspection of the current uncommitted diff, newly added split headers, adjacent C files, `docs/SPEC.md`, `docs/code-reading-guide.md`, `docs/protocol-model-review-guide.md`, and the Phase 4 work-plan section.
- Docker-based gates were not rerun in this review session. Parent verification reported normal build/load, bench build/load, C unit, static analysis, coverage + PHPT 15/15 lines 77.3% funcs 95.0%, fuzz 100, and PHPUnit 30/109.
