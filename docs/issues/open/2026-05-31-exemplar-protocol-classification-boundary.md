# お手本化: protocol classification と transport actionの境界整理

- Status: Open
- Created: 2026-05-31
- Branch: main
- Owner: Codex
- Parent: docs/issues/open/2026-05-31-c-php-extension-exemplar-structure.md

## Background

現在のresponse processingでは、metadata too large、message too large、invalid content-type、unsupported compression、malformed frameなどを検出する処理と、`RST_STREAM` 送信などのHTTP/2 transport actionが近い場所にある。

実装としては直接的だが、gRPC protocol classificationとHTTP/2 transport actionの責務境界を学ぶ教材としては読みにくい。

## Goals

- response parser / protocol classifierは「何が起きたか」を分類する責務として読めるようにする。
- transport layerは分類結果に基づいて `RST_STREAM`、connection dead、draining、cache detachを判断する責務として読めるようにする。
- status taxonomy、metadata shape、deadline、RST_STREAM / GOAWAY / EOF lifecycleの互換性を維持する。

## Non-Goals

- gRPC API互換性の変更。
- metadata/status/deadline semanticsの変更。
- performance改善。
- client streaming / bidi streaming対応。

## Plan

1. 現在のprotocol failure検出箇所とtransport side effect箇所を棚卸しする。
2. 分類結果の表現方法を設計する。
   - enum
   - flags
   - small result DTO
   - existing `grpc_call` flags維持
3. `RST_STREAM` を送る条件、connectionをdead/drainingにする条件、persistent cacheから外す条件を明文化する。
4. 実装する場合はsmall phaseに分け、各phaseでPHPT/PHPUnit/domain model reviewを通す。

## Performance Notes

response DATA chunk processingとmetadata callback pathに関数境界や分岐が増える可能性があるため、実装前にbefore計測を取る。

### Benchmark Policy

docs棚卸しだけならruntime benchmarkは不要。実装でresponse parser、metadata callback、status resolution、RST_STREAM送信条件、connection dead/draining判定のいずれかを変える場合は、before/afterを必須にする。

既存benchmarkでまず見るもの:

- `spanner-shape`
- `metadata-header`
- `./bench/run.sh cpu-micro --calls=20000 --warmup-calls=500`
- `./bench/run.sh payload-unary --payload-sizes=0,100,1024,102400`
- `./bench/run.sh payload-streaming --streams=20 --message-count=100 --payload-sizes=0,100,1024`
- parser/queueを動かす変更では `./bench/run.sh large-streaming`

既存benchmarkで足りない場合に追加するcase:

- invalid content-type、unsupported compression、malformed frame、message too large、metadata too largeを固定回数で流す `protocol-error-micro` または `cpu-micro` error/status caseを追加する。
- `RST_STREAM` 送信条件やconnection drainingに触る場合は、raw lifecycle fixture ports `50055`-`50060` のPHPTだけでなく、短い繰り返しcaseでtail regressionを確認する。
- DATA frame境界に依存するparser変更では、production benchmarkとは別に `PHP_GRPC_LITE_ENABLE_BENCH` diagnosticで fragmentation を見る。ただしbench buildはproduction layoutと異なるため、採否の主根拠にはしない。

採否は、互換性維持を最優先にする。小さい性能改善があっても、status taxonomyやRST_STREAM / GOAWAY / EOF lifecycleの説明が弱くなる変更は採用しない。

## Progress

- 2026-05-31: 親issueからprotocol classification boundary作業を子issue化。

## Verification

- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-coverage.sh`
- PHPUnit integration
- before/after benchmark
- HTTP/2/gRPC domain model review

## Decision Log

- 2026-05-31: この作業は教材価値が高いが、protocol behaviorとhot pathに触るため計測・レビュー必須とする。

## Close Criteria

- protocol failure分類とtransport actionの責務境界がdocsまたは実装上明確になっている。
- RST_STREAM / GOAWAY / EOF / invalid metadata / invalid content-type / compression / size limitの扱いが検証されている。
- before/after計測とdomain model reviewが記録されている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
