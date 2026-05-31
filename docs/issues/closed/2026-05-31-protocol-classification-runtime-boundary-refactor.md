# protocol classification runtime boundary refactor

- Status: Closed
- Created: 2026-05-31
- Branch: codex/protocol-classification-runtime-boundary-rejected-note
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
- 2026-05-31: `codex/protocol-classification-runtime-boundary-refactor` で `RST_STREAM` submit helper集約を試行し、ユーザー判断でreject。

## Trial Summary

試行ブランチ:

- `codex/protocol-classification-runtime-boundary-refactor`
- commit: `08a69ad protocol分類: RST_STREAM helperを集約`

試行内容:

- `nghttp2_submit_rst_stream()` の直接呼び出しを `grpc_lite_submit_rst_stream_if_open()` に集約した。
- helperは `session != NULL`、`call != NULL`、`stream_id > 0` のguardと `nghttp2_submit_rst_stream()` submitだけを担当する形にした。
- `src/server_streaming_call.c` からも使うため、`static` helperではなくinternal headerである `src/transport.h` に宣言を追加する形になった。

検証:

- `git diff --check`: pass
- `./tools/test/check-c-static-analysis.sh`: pass
- `./tools/test/check-c-unit.sh`: pass
- `./tools/test/check-phpt.sh`: pass, 15/15
- affected PHPUnit integration: pass, 16 tests / 50 assertions
- before/after benchmarkを実行
- HTTP/2/gRPC domain model review: 指摘なし

reject理由:

- このhelper化は、呼ばれるpathでは必ず `grpc_lite_submit_rst_stream_if_open()` の関数呼び出しを1段増やす。
- OK path中心のbenchでは明確な悪化は確認できなかったが、性能面で良くなる構造的な根拠は薄い。
- 可読性改善も限定的で、`nghttp2_submit_rst_stream()` のguardを直接読むより大きく見通しが良くなるとは判断しなかった。
- お手本プロジェクトとしては、性能に良くなる可能性がほぼなく、抽象化の利益も小さいruntime helperを採用しない方が妥当。

## Decision Log

- 2026-05-31: `docs/design/protocol-classification-boundary.md` の内容を即runtime変更へ進めるとhot pathに影響する可能性があるため、独立issueとしてbefore/after計測付きで扱う。
- 2026-05-31: `RST_STREAM` submit helper集約案はreject。実装ブランチはmergeしない。protocol classification boundaryは文書化された指針として残し、runtime codeは現状の直接呼び出しを維持する。

## Close Criteria

- 実装前の仮説、対象workload、before計測、採否基準が記録されている。
- 実装する場合はafter計測、互換性テスト、domain model reviewが記録されている。
- 採用/不採用の判断理由がDecision Logに残っている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
