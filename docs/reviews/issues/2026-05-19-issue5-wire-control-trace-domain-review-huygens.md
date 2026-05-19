# issue5 wire control trace domain review 2026-05-19

## Scope

- `ext/grpc/transport.c`
- `ext/grpc/bridge.c`
- `ext/grpc/tests/023-metadata-and-call-credentials.phpt`
- `tools/benchmark/spanner-commit-cli.php`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer

## Review Prompt Summary

- issue #5向けのwire diagnostic拡張、`x-goog-api-client` fold、metadata PHPT、Spanner Commit CLI fixtureについて、HTTP/2/gRPC domain modeling、metadata semantics、diagnostic vs production boundary、lifecycle/state transition、benchmark tooling boundaryを確認する。

## Issues

### REVIEW-20260519-001: outbound wire trace attributes stream-local frames to the I/O-driving call

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `wire.frame_out` の `rpc_method` が、HTTP/2 frameの `stream_id` に紐づく `grpc_call` ではなく `connection->current_io_call` から決まっている。connection-level frameなら「現在I/Oをdriveしているcall」として説明できるが、`RST_STREAM` / stream-level `WINDOW_UPDATE` / `HEADERS` / `DATA` など stream-local frame では、HTTP/2 Stream owner と I/O driver を混同している。
- Evidence: `ext/grpc/transport.c` の `grpc_lite_trace_outbound_frame()` は raw frame headerから `stream_id` をparseしているが、`call = connection->current_io_call` を `rpc_method` の sourceにしている。`send_pending_h2_frames()` は任意の `call` を `current_io_call` に入れてから `nghttp2_session_send()` を呼ぶため、別active stream向けにpendingされたcontrol frameもそのI/O driverのmethodとして記録され得る。
- Expected model: HTTP/2 Stream scoped frameのdiagnostic ownerは `stream_id` から `nghttp2_session_get_stream_user_data()` で引ける `grpc_call` であるべき。connection-level frameはconnection eventとして扱い、`rpc_method` を省くか、必要なら別名で `io_driver_rpc_method` のように表現する。
- Why it matters: issue #5のtraceはHEADERS/DATA/control frame混入とSpanner Commit request shapeの切り分けに使う一次資料である。active server streamingやread-ahead limit、PING ACK/RST/WINDOW_UPDATEのようなcontrol frameがある状況で、stream ownerとI/O driverがずれると、どのRPCがどのwire frameを出したかを誤って判断する。
- Recommended fix: `grpc_lite_trace_outbound_frame()` では `stream_id > 0` の場合に `grpc_call_from_stream_id(connection, stream_id)` を優先して `rpc_method` を出す。`stream_id == 0` の connection-level frameでは `rpc_method` を出さないか、`current_io_call` 由来の情報を別field名へ分ける。あわせてtrace docで `rpc_method` がstream ownerを意味することを固定する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `manual review only; no tests run`
- Notes: inbound control traceは `grpc_call_from_stream_id(connection, frame->hd.stream_id)` を使っており、この指摘はoutbound trace attributionに限定される。

### REVIEW-20260519-002: Spanner Commit CLI fixture needs an explicit diagnostic-vs-benchmark boundary

- Severity: `Design Decision`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `tools/benchmark/spanner-commit-cli.php` はreal Spanner Commitを繰り返し、wall-clock percentileを標準出力する専用CLIとしては有用だが、現在のbenchmark tooling contractである `bench/run.sh` / `bench/compare.sh` / OTEL span一次ソースから外れている。`tools/benchmark/` に置くなら、標準ベンチではなくissue #5のad-hoc diagnostic fixtureであることを明示する必要がある。
- Evidence: `tools/benchmark/spanner-commit-cli.php` は `SpannerClient` で実DBへ接続し、`runTransaction()` / `commit()` の `hrtime()` 差分を集計してtext出力する。一方、`docs/benchmarks/README.md` は通常比較の入口を `bench/compare.sh` とし、一次ソースを `otelop` へexportしたOTEL spanとしている。
- Expected model: benchmark toolingは比較対象、run id、suite boundary、OTEL exportを揃える。reporter reproductionやwire diagnostic用のsingle-purpose fixtureは、標準benchmarkと混ざらない名前・doc・配置で「診断fixture」として扱う。
- Why it matters: このCLIの数値を通常ベンチ結果として扱うと、ext-grpc比較線、OTEL集計、同条件再実行、Spanner emulator/real backendの揺れ幅の扱いが曖昧になる。issue #5の補助観測としては妥当でも、リポジトリのbenchmark contractを弱める。
- Recommended fix: どちらかを選ぶ。標準ベンチにするなら `bench/run.sh` suite化、`BenchTelemetry` / OTEL export、`BENCH_IMPLEMENTATION` 比較、docs記載を追加する。ad-hoc fixtureにするなら `tools/benchmark/README` やissue本文で「diagnostic/reproduction only; benchmark resultとして採用しない」と明記し、必要なら `tools/diagnostics/` 相当へ移す。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `manual review only; no tests run`
- Notes: CLI自体はproduction runtimeへ影響せず、real Spanner Commit pathを直接触れるfixtureとしては有用。問題は配置と結果解釈のdomain boundary。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `1`
- Low: `none`
- Design Decision: `1`

## Fix Update 2026-05-19

### REVIEW-20260519-001

- Status: `Closed`
- Fix summary: outbound `wire.frame_out` now resolves `rpc_method` from the HTTP/2 `stream_id` owner via `grpc_call_from_stream_id()` rather than `connection->current_io_call`. Connection-level `stream_id == 0` frames do not get `rpc_method`.
- Verification: `./tools/test/check-phpt.sh ext/grpc/tests/029-trace-file.phpt ext/grpc/tests/023-metadata-and-call-credentials.phpt`; `./tools/test/check-c-static-analysis.sh`.

### REVIEW-20260519-002

- Status: `Closed`
- Fix summary: moved the fixture to `tools/diagnostics/spanner-transaction-cli.php`, added `tools/diagnostics/README.md`, and documented that it is an issue reproduction/diagnostic fixture, not a standard benchmark or Commit-only measurement.
- Verification: `docker compose run --rm dev php -l /workspace/tools/diagnostics/spanner-transaction-cli.php`; real Spanner smoke with 5 iterations.
