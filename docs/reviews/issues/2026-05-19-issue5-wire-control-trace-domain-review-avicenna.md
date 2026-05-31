# issue #5 wire/control trace domain review 2026-05-19

## Scope

- `ext/grpc/transport.c`
- `ext/grpc/bridge.c`
- `ext/grpc/tests/023-metadata-and-call-credentials.phpt`
- `ext/grpc/tests/029-trace-file.phpt`
- `tools/benchmark/spanner-commit-cli.php`
- `docs/verification/protocol-model-review-guide.md`

## Reviewer Role

- HTTP/2/gRPC domain model reviewer for issue #5 wire diagnostics and metadata semantics

## Review Prompt Summary

- 最新のwire diagnostics、`x-goog-api-client` folding、trace PHPT、Spanner Commit CLI fixtureについて、HTTP/2/gRPC domain model、metadata semantics、diagnostic vs production boundary、lifecycle/state transitions、benchmark tooling境界を確認した。ソースコードは変更しない。

## Issues

### REVIEW-20260519-001: outbound connection-level frames can be attributed as an RPC frame

- Severity: `High`
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model reviewer for issue #5 wire diagnostics and metadata semantics`
- Finding: `grpc_lite_trace_outbound_frame()` は `connection->current_io_call` があると、`stream_id == 0` の `PING` / `SETTINGS` / `WINDOW_UPDATE` などconnection-level frameにも `rpc_method` を付ける。これはHTTP/2 Connection scopeのcontrol frameをgRPC Call scopeへ写してしまう。
- Evidence: `ext/grpc/transport.c` `grpc_lite_trace_outbound_frame()` / `send_pending_h2_frames()`
- Expected model: `stream_id == 0` のcontrol frameはHTTP/2 Connectionのstate transitionであり、特定RPCのrequest HEADERS/DATAとは別scopeとして記録する。RPCとの関係が必要な場合も、`rpc_method` ではなく `trigger_rpc_method` のような診断上の因果ヒントとして分離する。
- Why it matters: issue #5は「small TLS writeがCommit requestに属するか」を切り分ける調査であり、connection-level PING ACKやWINDOW_UPDATEにRPC methodを付けると、過去の39B write誤帰属と同じ種類の分析ミスを再発させる。
- Recommended fix: `stream_id == 0` のframeには `rpc_method` を付けない。必要なら `trigger_rpc_method` / `current_io_method` など明示的にconnection-level frameの送信契機として別フィールドにする。PHPTもconnection-level frameにRPC methodが付かないことを固定する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `manual domain review only`
- Notes: inbound control traceは `grpc_call_from_stream_id()` を通しており、stream 0にはRPC methodを付けないためdomain modelに近い。

### REVIEW-20260519-002: outbound frame trace parses only one frame per send callback chunk

- Severity: `High`
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model reviewer for issue #5 wire diagnostics and metadata semantics`
- Finding: `grpc_lite_trace_outbound_frame()` は `send_callback()` の `data,length` を単一HTTP/2 frameとして解釈し、先頭9Bだけからframe type/lengthを読む。nghttp2 send callback chunkは診断上「wire bytes chunk」であり、複数frameまたはconnection preface + frameを含み得るため、traceが後続frameを落とす可能性がある。
- Evidence: `ext/grpc/transport.c` `send_callback()` / `grpc_lite_trace_outbound_frame()`
- Expected model: wire diagnosticsはHTTP/2 frame streamを表す必要がある。send callback chunkはframe boundaryそのものではないため、connection prefaceを消費した後、chunk内をHTTP/2 frame header lengthで反復して各frameを記録する。
- Why it matters: issue #5のwire-shape比較ではHEADERS/DATA/control framesの有無・順序・サイズが一次資料になる。chunk先頭frameだけを記録すると、HEADERS/DATA coalescing、PING ACK、WINDOW_UPDATE、SETTINGS ACKの有無を誤判定する。
- Recommended fix: outbound traceをchunk parserにし、preface部分と0個以上のHTTP/2 framesを順にemitする。不完全frameが来た場合は `wire.chunk_unparsed` としてchunk length/offsetを記録し、frameとして断定しない。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `manual domain review only`
- Notes: 現在のPHPTは期待frameが見えることだけを確認しており、chunk内複数frameの完全性は検証していない。

### REVIEW-20260519-003: x-goog-api-client folding is a Google API metadata exception to generic gRPC duplicate semantics

- Severity: `Design Decision`
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model reviewer for issue #5 wire diagnostics and metadata semantics`
- Finding: `grpc_lite_merge_call_credentials_metadata()` は `x-goog-api-client` だけを既存request metadataとCallCredentials metadataでspace-joined single valueへfoldする。これはphp-grpc-liteの一般方針である「gRPC metadata duplicate valuesを `array<string,list<string>>` として保持する」と異なるGoogle API固有例外である。
- Evidence: `ext/grpc/bridge.c` `grpc_lite_fold_x_goog_api_client()`、`ext/grpc/tests/023-metadata-and-call-credentials.phpt`
- Expected model: generic gRPC metadata semanticsとGoogle API/GAX compatibility exceptionは明示的に分離され、どのkeyにだけ適用するか、request metadata単体のduplicatesとCallCredentials merge時のduplicatesで挙動が違う理由が記録されている。
- Why it matters: metadata duplicate preservationはSPECで明確化済みのdomain decisionであり、特定keyのfoldingはwire-shape比較には有用でも、通常metadata semanticsへ暗黙に混ざると将来の互換性判断を誤る。
- Recommended fix: SPECまたはissue #5 tracking docへ、`x-goog-api-client` foldingはGoogle API client metrics headerのwire compatibility exceptionであり、CallCredentials merge境界に限定することを明記する。可能なら関数名にも `google_api_client_metrics` などdomain-specific性を出す。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `manual domain review only`
- Notes: 現行テストはrequest value + credentials valueが1 metadata valueへfoldされることをwire-visibleに確認しており、挙動自体の固定としては妥当。

### REVIEW-20260519-004: spanner-commit-cli fixture does not isolate Commit wire shape

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model reviewer for issue #5 wire diagnostics and metadata semantics`
- Finding: `tools/benchmark/spanner-commit-cli.php` は名前上Commit fixtureだが、各iterationの測定範囲は `runTransaction()` 全体で、transaction内に `SELECT @i` と `commit()` を含む。Commit RPC単体のwire shapeやlatencyを切り出すfixtureではない。
- Evidence: `tools/benchmark/spanner-commit-cli.php`
- Expected model: issue #5のwire comparison fixtureは、Spanner Commit RPCのHTTP/2 stream単位を識別できるか、少なくとも「transaction全体」「Commit method単体」「pre-Commit SELECT」を名前と出力で区別する。
- Why it matters: `Commit` latency差の調査でtransaction全体のCLI結果をCommit単体として扱うと、SELECT server-processing、Begin/Commit lifecycle、session state、client library transaction orchestrationが混ざり、wire-shape比較の一次資料として使えない。
- Recommended fix: このCLIをbenchmark toolingに置くなら、名前/出力を `spanner-transaction-commit-cli.php` 相当にするか、trace markerで `/google.spanner.v1.Spanner/Commit` のmethod-level windowを明示する。Commit-only比較には、Commit stream id・request bytes・response bytesをtraceから抽出する前提をREADMEまたはscript usageに書く。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `manual domain review only`
- Notes: benchmark toolingに置くこと自体は、real Spanner diagnostic fixtureとしては妥当。ただし現状の名前と集計粒度はissue #5のwire-shape目的より広い。

## Review Result

- Blocker: `none`
- High: `2`
- Medium: `1`
- Low: `none`
- Design Decision: `1`

## Fix Update 2026-05-19

### REVIEW-20260519-001

- Status: `Closed`
- Fix summary: connection-level outbound frames (`stream_id == 0`) no longer carry `rpc_method`. PHPT now asserts outbound SETTINGS and connection WINDOW_UPDATE are not attributed to an RPC.
- Verification: `./tools/test/check-phpt.sh ext/grpc/tests/029-trace-file.phpt ext/grpc/tests/023-metadata-and-call-credentials.phpt`; `./tools/test/check-c-static-analysis.sh`.

### REVIEW-20260519-002

- Status: `Closed`
- Fix summary: outbound trace now parses a send-callback chunk as HTTP/2 wire bytes: it consumes the connection preface, iterates all complete HTTP/2 frames in the chunk, emits one `wire.frame_out` record per frame, and emits `wire.chunk_unparsed` if bytes remain incomplete.
- Verification: `./tools/test/check-phpt.sh ext/grpc/tests/029-trace-file.phpt`; local wire smoke confirmed SETTINGS / WINDOW_UPDATE / HEADERS / DATA / PING records.

### REVIEW-20260519-003

- Status: `Closed`
- Fix summary: issue tracking doc records `x-goog-api-client` folding as a Google API client metrics header exception limited to the CallCredentials merge boundary, while generic duplicate gRPC metadata remains preserved.
- Verification: `./tools/test/check-phpt.sh ext/grpc/tests/023-metadata-and-call-credentials.phpt`.

### REVIEW-20260519-004

- Status: `Closed`
- Fix summary: renamed/moved the script to `tools/diagnostics/spanner-transaction-cli.php`, added stderr and README warnings that the fixture measures full transaction wall time, not Commit RPC alone, and documented that per-RPC analysis must use `GRPC_LITE_TRACE_FILE` records.
- Verification: `docker compose run --rm dev php -l /workspace/tools/diagnostics/spanner-transaction-cli.php`; real Spanner smoke with 5 iterations.
