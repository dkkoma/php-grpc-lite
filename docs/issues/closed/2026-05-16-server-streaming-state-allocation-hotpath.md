---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# server streaming state の未使用保持を削減する

## 目的

server streaming call開始時のC拡張側allocation / zval refcount固定費を削減する。

## 背景

`server_streaming_call_state` は、stream開始後に使わない `path` と `metadata` を保持している。現在のproduction経路では、HTTP/2 request submit後の再送は行わず、production stream lifecycleに持ち越す必要がない。bench diagnostic buildではmethod pathを診断recordへ使うため、production stateとは分けて扱う。

SpannerのSELECT/DMLはserver streaming call経路を通るため、call開始時固定費として効く可能性がある。

## スコープ

- `server_streaming_call_state` からproduction経路で未使用の `path` と `metadata` を削除する。
- destructor側の解放処理も削除する。
- server streaming PHPTとreal Spanner mixed transactionで影響を確認する。

## 非スコープ

- request payload ownershipの変更。
- response queue / read-ahead戦略の変更。
- HTTP/2 stream lifecycleの変更。

## 計画

1. `server_streaming_call_state` の未使用fieldを削除する。
2. `server_streaming_call_open_resource()` のallocation/copyを削除する。
3. PHPT / static analysisを通す。
4. real Spanner mixed c16でCPU/requestを確認する。

## 進捗

- Issue作成。

## 検証

未実施。

## 判断ログ

未判断。

## 完了条件

- 未使用fieldがproduction stateから消える。
- PHPT / static analysisが通る。
- real Spanner mixed c16の結果を記録する。

## 2026-05-16 実装

- `server_streaming_call_state` からproduction経路で未使用の `path` と `metadata` を削除した。
- `server_streaming_call_open_resource()` のproduction向け `path` copyとmetadata zval copyを削除した。
- bench diagnostic buildでは `rpc_service` / `rpc_method` 生成にmethod pathが必要なため、`path` だけ `PHP_GRPC_LITE_ENABLE_BENCH` 限定で保持する。
- `destroy_server_streaming_call_state()` の対応する解放処理もproduction/bench境界に合わせて整理した。

## 検証結果

### PHPT / static

- `ext/grpc/tests/011-server-streaming.phpt ext/grpc/tests/020-request-metadata-control.phpt ext/grpc/tests/024-control-semantics.phpt`: PASS。
- `./tools/test/check-phpt.sh`: PASS 15/15。
- `./tools/test/check-c-static-analysis.sh`: PASS。

### real Spanner mixed transaction

Command: `BENCH_RUN_ID=cpu-stream-state-real-mixed-c16-compare-20260516 BENCH_VARIANTS='native ext-grpc' BENCH_ACTIONS='transaction_select2_update1_insert1' ... ./bench/fpm-laravel-spanner-load-compare.sh 256 16`

| variant | cpu_us/req | rps | avg_ms | p50_ms | p90_ms | max_ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| native | 9963.3 | 27.8507 | 564.9 | 564.2 | 638.3 | 742.4 |
| ext-grpc | 11878.5 | 22.4233 | 701.5 | 650.4 | 972.1 | 1499.1 |

## 判断

- Status: Closed
- Decision: Adopted

stream resource lifetimeに不要なmetadata zvalを持たない変更であり、production経路ではmethod pathも保持しない。bench diagnostic buildではmethod pathが診断record生成の責務として必要なため、bench限定stateとして残した。server streaming lifecycleとcontrol semanticsのPHPTで問題は出ていない。効果は小さい可能性が高いが、責務分離と固定費削減として採用する。
