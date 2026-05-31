# protocol classification runtime boundary refactor

- Status: Closed
- Created: 2026-05-31
- Branch: codex/protocol-classification-runtime-boundary-refactor
- Owner: Codex
- Related-Design: docs/design/protocol-classification-boundary.md
- Parent: docs/issues/open/2026-05-31-c-php-extension-exemplar-structure.md

## Background

`docs/design/protocol-classification-boundary.md` で、gRPC protocol failureのclassification、HTTP/2 transport action、status taxonomy、PHP result bridgeの責務境界を文書化した。

現行実装では、`src/transport.c` のhot path内でclassification flag更新と `RST_STREAM` 送信が近い場所にある。挙動としては検証済みだが、実装構造としては「何が起きたか」と「HTTP/2 lifecycleをどう動かすか」の境界がまだコード上で読み取りにくい。

このissueでは、文書化したboundaryを小さいruntime refactorとして実装できるかを検証する。性能や互換性に悪影響が出る場合は採用せず、調査結果として閉じる。

## Goals

- protocol classificationとtransport actionの境界を、コード上でも読みやすくする。
- `RST_STREAM(CANCEL)` 送信条件を小さいtransport action helperへ寄せる。
- `status_core.c` のstatus taxonomy priorityは変えない。
- metadata/status/deadline/RST_STREAM/GOAWAY/EOF lifecycleの互換性を維持する。
- before/after benchmarkとdomain model reviewで採否を判断する。

## Non-Goals

- gRPC API互換性の変更。
- status code priorityの変更。
- retry policyやidempotency判断の追加。
- connection reuse policyの変更。
- client streaming / bidi streaming対応。
- performance改善そのものを目的にしない。

## Proposed Scope

最初の候補は、挙動変更を避けた小さいhelper抽出に限定する。

1. `RST_STREAM(CANCEL)` の送信を `grpc_lite_cancel_stream_if_open()` のようなtransport action helperへ集約する。
   - 入力: `nghttp2_session *session`, `grpc_call *call`, `uint32_t error_code`
   - 責務: `session != NULL`、`call != NULL`、`stream_id > 0` のguardと `nghttp2_submit_rst_stream()`
   - 非責務: status flag設定、connection dead判定、details文字列生成
2. response DATA parser内のclassification flag設定は維持する。
   - `response_message_too_large`
   - `compressed_response_seen`
   - `malformed_response_frame`
   - `response_queue_limit_exceeded`
3. metadata size超過時のstream cancelも同じhelperを使う。
4. `PUSH_PROMISE` rejectionやserver streaming destructor cancelなど、`RST_STREAM` error codeが `CANCEL` 以外の箇所は同helperで扱えるかを判断する。

この範囲で分岐追加や関数呼び出し増加がhot pathに悪影響を出す場合は、不採用にする。

## Performance Plan

実装前にbeforeを取る。

必須:

- `./bench/run.sh cpu-micro --calls=20000 --warmup-calls=500 --repeat-runs=3`
- `./bench/run.sh metadata-header`
- `./bench/run.sh payload-unary --payload-sizes=0,100,1024,102400`
- `./bench/run.sh payload-streaming --streams=20 --message-count=100 --payload-sizes=0,100,1024`

parser/queue helper境界を動かす場合:

- `./bench/run.sh large-streaming`

既存benchmarkだけでerror pathの判断が弱い場合:

- invalid content-type、unsupported compression、malformed frame、message too large、metadata too largeを固定回数で見る `protocol-error-micro` 相当を追加する。
- 追加benchは性能改善目的ではなく、classification/action境界変更のregression検出用にする。

採否基準:

- OK pathの `cpu-micro`、metadata、payload系がbaselineから悪化しない。
- error/control PHPTでstatus code、details、metadata shape、connection recoveryが変わらない。
- コードが「classification」と「transport action」を明確に分けて読める。
- 効果が読みやすさに対して小さい、またはhot path悪化が見える場合は採用しない。

## Verification Plan

- `git diff --check`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`
- affected PHPUnit integration:
  - `tests/Integration/CompressionTest.php`
  - `tests/Integration/ControlSemanticsTest.php`
  - `tests/Integration/HttpValidationTest.php`
  - `tests/Integration/MetadataCompatibilityTest.php`
- before/after benchmark
- HTTP/2/gRPC domain model review

## Progress

- 2026-05-31: docs-only boundary整理の後続runtime refactor issueとして作成。
- 2026-05-31: issue専用ブランチ `codex/protocol-classification-runtime-boundary-refactor` を作成。
- 2026-05-31: before/after benchmark、互換性テスト、domain model reviewまで完了。

## Implementation Hypothesis

最初の実装候補は、`nghttp2_submit_rst_stream()` 呼び出しを小さいtransport action helperへ寄せるだけに限定する。

仮説:

- `session != NULL`、`call != NULL`、`stream_id > 0` のguardと `nghttp2_submit_rst_stream()` が各classification pathへ散っているため、helper化すると「classification flag設定」と「stream cancel action」の境界が読みやすくなる。
- helperはできるだけ小さく保ち、header/struct layoutは変えない。
- status flag、status taxonomy、connection dead/draining、cache detach、details文字列は変えない。
- hot pathに関数呼び出しが増えるため、before/afterで悪化が見える場合は採用しない。

採否基準:

- `cpu-micro`、`metadata-header`、payload系、large streamingでbaselineから悪化しない。
- PHPT、C unit、static analysis、対象PHPUnitが通る。
- domain model reviewでtransport action helperがclassification responsibilityを持っていないことを確認する。

## Before Measurements

Planned run ids:

- `protocol-boundary-before-cpu-micro-20260531`
- `protocol-boundary-before-metadata-header-20260531`
- `protocol-boundary-before-payload-unary-20260531`
- `protocol-boundary-before-payload-streaming-20260531`
- `protocol-boundary-before-large-streaming-20260531`

Results:

| Suite | Shape | Before | Notes |
|---|---:|---:|---|
| `cpu-micro` | `tiny_unary_0b` | 11.0 us CPU/call median | 3 repeats |
| `cpu-micro` | `small_unary_100b` | 11.1 us CPU/call median | 3 repeats |
| `cpu-micro` | `new_client_unary_100b` | 13.4 us CPU/call median | 3 repeats |
| `cpu-micro` | `metadata_unary_req10_resp10_32b` | 16.2 us CPU/call median | 3 repeats |
| `cpu-micro` | `begin_txn_unary` | 11.6 us CPU/call median | 3 repeats |
| `cpu-micro` | `commit_txn_unary` | 11.6 us CPU/call median | 3 repeats |
| `cpu-micro` | `small_streaming_1x100b` | 12.2 us CPU/call median | 3 repeats |
| `cpu-micro` | `tiny_streaming_1x0b` | 12.0 us CPU/call median | 3 repeats |
| `cpu-micro` | `new_client_streaming_1x100b` | 14.2 us CPU/call median | 3 repeats |
| `cpu-micro` | `small_streaming_10x100b` | 17.1 us CPU/call median | 3 repeats |
| `cpu-micro` | `small_streaming_100x100b` | 63.5 us CPU/call median | 3 repeats |
| `cpu-micro` | `select_1row_10col_streaming` | 12.5 us CPU/call median | 3 repeats |
| `cpu-micro` | `dml_insert_10col_streaming` | 12.6 us CPU/call median | 3 repeats |
| `cpu-micro` | `dml_update_10col_streaming` | 12.6 us CPU/call median | 3 repeats |
| `cpu-micro` | `dml_delete_10col_streaming` | 12.6 us CPU/call median | 3 repeats |
| `metadata-header` | req0/resp0 | p50 29.1 us / p99 13077.6 us | p99は外れ値あり |
| `metadata-header` | req10/resp0 | p50 38.2 us / p99 738.6 us |  |
| `metadata-header` | req10/resp10 | p50 60.3 us / p99 1425.8 us |  |
| `metadata-header` | req50/resp0 | p50 179.2 us / p99 3262.9 us |  |
| `metadata-header` | req50/resp50 | p50 198.0 us / p99 1702.6 us |  |
| `payload-unary` | 1024B | p50 28.5 us / p99 101.2 us | summary出力は1024B/102400Bのみ |
| `payload-unary` | 102400B | p50 79.8 us / p99 1966.8 us | summary出力は1024B/102400Bのみ |
| `payload-streaming` | 0B x100 | p50 134.1 us / p99 3871.4 us | 20 streams |
| `payload-streaming` | 100B x100 | p50 161.8 us / p99 1896.5 us | 20 streams |
| `payload-streaming` | 1024B x100 | p50 224.8 us / p99 1715.7 us | 20 streams |
| `large-streaming` | 10000 x 100B | p50 16663.1 us | count 1 |
| `large-streaming` | 100000 x 100B | p50 100427.1 us | count 1 |

## Implementation Notes

- `nghttp2_submit_rst_stream()` の直接呼び出しを `grpc_lite_submit_rst_stream_if_open()` に集約した。
- 当初は `src/transport.c` 内の `static` helperを想定したが、`src/server_streaming_call.c` のserver streaming resource destructor cancelでも同じtransport actionを使うため、private internal headerである `src/transport.h` へ宣言を置いた。
- helperの責務は `session != NULL`、`call != NULL`、`stream_id > 0` のguardと `nghttp2_submit_rst_stream()` 呼び出しだけに限定した。
- classification flag、status taxonomy、connection dead/draining、persistent cache detach、details文字列は変更していない。

## After Measurements

Run ids:

- `protocol-boundary-after-cpu-micro-20260531`
- `protocol-boundary-after-metadata-header-20260531`
- `protocol-boundary-after-payload-unary-20260531`
- `protocol-boundary-after-payload-streaming-20260531`
- `protocol-boundary-after-large-streaming-20260531`

Results:

| Suite | Shape | Before | After | Judgment |
|---|---:|---:|---:|---|
| `cpu-micro` | `tiny_unary_0b` | 11.0 | 10.9 | 中立 |
| `cpu-micro` | `small_unary_100b` | 11.1 | 11.2 | 中立 |
| `cpu-micro` | `new_client_unary_100b` | 13.4 | 13.4 | 中立 |
| `cpu-micro` | `metadata_unary_req10_resp10_32b` | 16.2 | 15.9 | 中立 |
| `cpu-micro` | `begin_txn_unary` | 11.6 | 11.4 | 中立 |
| `cpu-micro` | `commit_txn_unary` | 11.6 | 11.4 | 中立 |
| `cpu-micro` | `small_streaming_1x100b` | 12.2 | 12.3 | 中立 |
| `cpu-micro` | `tiny_streaming_1x0b` | 12.0 | 12.1 | 中立 |
| `cpu-micro` | `new_client_streaming_1x100b` | 14.2 | 14.5 | 小さいregression傾向だがnoise範囲 |
| `cpu-micro` | `small_streaming_10x100b` | 17.1 | 17.1 | 中立 |
| `cpu-micro` | `small_streaming_100x100b` | 63.5 | 64.6 | 小さいregression傾向だがnoise範囲 |
| `cpu-micro` | `select_1row_10col_streaming` | 12.5 | 12.7 | 中立 |
| `cpu-micro` | `dml_insert_10col_streaming` | 12.6 | 12.6 | 中立 |
| `cpu-micro` | `dml_update_10col_streaming` | 12.6 | 12.6 | 中立 |
| `cpu-micro` | `dml_delete_10col_streaming` | 12.6 | 12.6 | 中立 |
| `metadata-header` | req0/resp0 | p50 29.1 / p99 13077.6 | p50 104.7 / p99 13783.5 | p50はregression傾向、同classに大きな外れ値あり |
| `metadata-header` | req10/resp0 | p50 38.2 / p99 738.6 | p50 49.2 / p99 280.6 | mixed |
| `metadata-header` | req10/resp10 | p50 60.3 / p99 1425.8 | p50 44.4 / p99 697.0 | 改善 |
| `metadata-header` | req50/resp0 | p50 179.2 / p99 3262.9 | p50 131.3 / p99 2378.6 | 改善 |
| `metadata-header` | req50/resp50 | p50 198.0 / p99 1702.6 | p50 203.8 / p99 2150.6 | 中立から小さいregression |
| `payload-unary` | 1024B | p50 28.5 / p99 101.2 | p50 28.5 / p99 109.0 | 中立 |
| `payload-unary` | 102400B | p50 79.8 / p99 1966.8 | p50 77.8 / p99 1932.9 | 中立 |
| `payload-streaming` | 0B x100 | p50 134.1 / p99 3871.4 | p50 113.1 / p99 1828.7 | 改善 |
| `payload-streaming` | 100B x100 | p50 161.8 / p99 1896.5 | p50 152.7 / p99 434.5 | 改善 |
| `payload-streaming` | 1024B x100 | p50 224.8 / p99 1715.7 | p50 197.5 / p99 2603.8 | mixed |
| `large-streaming` | 10000 x 100B | p50 16663.1 | p50 13060.9 | 改善 |
| `large-streaming` | 100000 x 100B | p50 100427.1 | p50 96498.6 | 改善 |

Interpretation:

- `cpu-micro` は一部streaming caseで +1.7% から +2.1% 程度のregression傾向の値があるが、payload/large streamingでは同方向の悪化が再現していない。
- `metadata-header` はrunごとの外れ値が大きく、req0/resp0のp50だけで採否を決めるには弱い。metadataが増えるcaseでは改善または中立に寄っている。
- `payload-unary` は中立、`payload-streaming` と `large-streaming` はおおむね改善または中立で、RST_STREAM helper化による明確なruntime悪化は確認できなかった。

## Verification Results

- `git diff --check`: pass
- `./tools/test/check-c-static-analysis.sh`: pass
- `./tools/test/check-c-unit.sh`: pass
- `./tools/test/check-phpt.sh`: pass, 15/15
- affected PHPUnit integration: pass, 16 tests / 50 assertions

## Domain Model Review

- Review: `docs/reviews/issues/2026-05-31-protocol-boundary-rst-helper-domain-review-codex.md`
- Result: Blocker / High / Medium / Low / Design Decision はすべて none。
- 確認結果:
  - `grpc_lite_submit_rst_stream_if_open()` はtransport actionのguardと `nghttp2_submit_rst_stream()` submitだけを担当している。
  - classification flags、status taxonomy、connection dead/draining、cache detach、PHP-visible details生成はhelperへ移していない。
  - GOAWAYとinbound RST_STREAM handlingはoutbound stream-local cancellationとは別のlifecycleとして残っている。

## Decision Log

- 2026-05-31: `docs/design/protocol-classification-boundary.md` の内容を即runtime変更へ進めるとhot pathに影響する可能性があるため、独立issueとしてbefore/after計測付きで扱う。
- 2026-05-31: helperは当初 `static` を想定したが、server streaming resource destructor cancelにも同じtransport actionがあるため、private internal headerである `src/transport.h` に宣言する形を採用した。public PHP API surfaceは増やしていない。
- 2026-05-31: after benchmarkでは `cpu-micro` の一部streaming caseに小さいregression傾向が出たが、payload/large streamingでは同方向の悪化が再現せず、互換性テストとdomain model reviewも通った。helper化による明確なruntime悪化は確認できないため採用する。

## Fix Commit

- This branch commit.

## Close Criteria

- 実装前の仮説、対象workload、before計測、採否基準が記録されている。
- 実装する場合はafter計測、互換性テスト、domain model reviewが記録されている。
- 採用/不採用の判断理由がDecision Logに残っている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
