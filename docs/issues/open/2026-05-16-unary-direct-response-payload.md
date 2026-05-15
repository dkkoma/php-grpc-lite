---
Status: Open
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
