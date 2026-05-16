---
Status: Closed
Owner: Codex
Created: 2026-05-16
Branch: main
---

# unary responseをgRPC frame込みbody経由からdirect payload deliveryへ移す

## 目的

unary response pathの余分な `smart_str body`、body copy、payload extraction copyを削減する。

## 背景

現行unary pathはHTTP/2 DATAを `smart_str body` に蓄積し、`typed_result->body` にコピーし、bridge側でgRPC 5B headerを検証してpayloadだけを新しい `zend_string` にコピーしている。server streaming pathにはdirect response payload assemblyがあり、unaryも同じ考え方に寄せられる。

## スコープ

- unary pathで `decode_response_incrementally` / `direct_response_payload` 相当を使う。
- unaryでは最大1 message制約を維持する。
- status/error/metadata/deadline semanticsを変えない。

## 非スコープ

- server streaming delivery strategyの変更。
- protobuf decodeの変更。

## 検証

- PHPT / PHPUnit。
- Go test-server主要bench。
- real Spanner single select / single DML / mixed transaction c16。

## 完了条件

- unary successful responseでframe込みbodyを中間保持しない。
- CPU/requestとwall timeへの影響が記録される。

## 試行内容

- `grpc_lite_unary_result` のbody保持をpayload保持に変え、unary responseでも既存のincremental/direct response parserにpayloadをqueueさせる形を試した。
- bridge側のgRPC 5B frame再検証とpayload再コピーを削除し、queued payloadのownershipをそのまま `StartBatch` resultへ渡す形にした。

## 検証結果

- PHPT:
  - `tests/010-unary.phpt`: PASS
  - `tests/021-deadline.phpt`: PASS
  - `tests/022-error-and-http-validation.phpt`: PASS
  - `tests/025-resource-limits.phpt`: PASS
  - `tests/011-server-streaming.phpt`: PASS
  - `tests/024-control-semantics.phpt`: PASS
- real Spanner Laravel/FPM mixed transaction:
  - 条件: `transaction_select2_update1_insert1`, 256 requests, concurrency 16, 16 FPM workers, cloud Spanner
  - 結果: `cpu_us/req=11504.9`, `rps=25.5848`, `avg_ms=612.4`, `p50_ms=614.0`, `p90_ms=706.4`
  - 直前のchannel key cache後のgrpc-lite結果 `cpu_us/req=11431.5` に対して改善なし。

## 判断

- 採用しない。
- コピー削減としては妥当な方向だが、対象ワークロードではCPU/requestの改善が観測できず、direct parserをunary successful responseに常時使うことで成功経路の状態管理が増える。
- 本候補は実装を取り込まず、現行unary body経路を維持する。

## Fix summary

- 実装差分は破棄。
- issueには試行結果だけを残す。
