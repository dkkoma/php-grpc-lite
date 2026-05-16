---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# request metadata copy をCOW化してStartBatch固定費を下げる

## 目的

`Call::startBatch()` の `OP_SEND_INITIAL_METADATA` 保存時に、metadata HashTableを毎回deep copyする固定費を下げる。

## 背景

real Spanner + GAX経路では、RPCごとに `x-goog-api-client`、`x-goog-request-params`、`authorization` など複数のrequest metadataが作られる。C拡張側では `grpc_lite_store_send_batch()` が送信前metadataを `grpc_lite_copy_metadata()` でHashTable copyしてから、native transport実行時にHTTP/2 headerへ変換している。

PHP配列はcopy-on-writeなので、送信batchで受け取ったmetadata配列をCOW参照として保持すれば、通常ケースのHashTable copyを避けられる可能性がある。

## スコープ

- `grpc_lite_copy_metadata()` をHashTable deep copyからzval COW copyへ変更する。
- request metadataについて、`startBatch()` 後に元配列を変更してもwireへ漏れないことをPHPTで確認する。
- response metadata / status metadataの既存互換性が壊れないことをPHPTで確認する。
- real Spanner mixed transactionでCPU/requestへの影響を確認する。

## 非スコープ

- `BaseStub::_validate_and_normalize_metadata()` のPHP userland最適化。
- HTTP/2 header変換処理の仕様変更。
- metadata validation / filtering仕様の変更。

## 計画

1. `grpc_lite_copy_metadata()` をCOW参照保持に変更する。
2. raw `Grpc\Call::startBatch()` でmetadata mutation isolationをテストする。
3. PHPT / static analysisを通す。
4. real Spanner mixed c16で前回値と比較する。
5. 採用可否を判断する。

## 進捗

- Issue作成。

## 検証

未実施。

## 判断ログ

未判断。

## 完了条件

- metadata mutation isolationのテストがある。
- PHPT / static analysisが通る。
- real Spanner mixed transactionのCPU/request結果を記録し、採用/棄却を判断する。

## 2026-05-16 実装

- `grpc_lite_copy_metadata()` をHashTable deep copyから `ZVAL_COPY()` に変更し、metadata配列をCOW参照として保持するようにした。
- COW配列へ書き込む可能性がある `grpc_lite_append_user_agent()` と `grpc_lite_merge_call_credentials_metadata()` は `SEPARATE_ARRAY()` 後に変更するようにした。
- raw `Grpc\Call::startBatch()` で、`OP_SEND_INITIAL_METADATA` 後に元metadata配列を変更してもwireへ漏れないことをPHPTに追加した。

## 検証結果

### PHPT / static

- `ext/grpc/tests/020-request-metadata-control.phpt`: PASS。
- `ext/grpc/tests/020-request-metadata-control.phpt ext/grpc/tests/023-metadata-and-call-credentials.phpt ext/grpc/tests/026-franken-go-backend.phpt`: PASS。
- `./tools/test/check-phpt.sh`: PASS 15/15。
- `./tools/test/check-c-static-analysis.sh`: PASS。

### metadata-header

Command: `BENCH_TAG=cpu-metadata-cow-20260516-095354 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/compare.sh metadata-header --calls=500`

| measurement | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc p50 | ext-grpc p99 |
| --- | ---: | ---: | ---: | ---: |
| `metadata_header_req_0_resp_0_value_0b` | 55.2µs | 728.5µs | 77.4µs | 732.9µs |
| `metadata_header_req_10_resp_0_value_32b` | 57.0µs | 452.8µs | 88.6µs | 308.0µs |
| `metadata_header_req_10_resp_10_value_32b` | 52.0µs | 212.9µs | 102.9µs | 460.7µs |
| `metadata_header_req_50_resp_0_value_32b` | 102.8µs | 381.6µs | 122.0µs | 398.7µs |
| `metadata_header_req_50_resp_50_value_32b` | 175.2µs | 1095.1µs | 242.2µs | 972.8µs |

### real Spanner mixed transaction

Command: `BENCH_RUN_ID=cpu-metadata-cow-real-mixed-c16-compare-20260516 BENCH_VARIANTS='native ext-grpc' BENCH_ACTIONS='transaction_select2_update1_insert1' ... ./bench/fpm-laravel-spanner-load-compare.sh 256 16`

| variant | cpu_us/req | rps | avg_ms | p50_ms | p90_ms | max_ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| native | 10508.6 | 23.9329 | 618.7 | 581.9 | 1084.3 | 1831.1 |
| ext-grpc | 11465.9 | 26.1772 | 568.6 | 549.7 | 888.8 | 1757.1 |

参考: 直前のnative単独runでは `cpu_us/req=9036.6`。Spanner条件の揺れはあるが、COW化による悪化は見えない。

## レビュー

- `docs/reviews/issues/2026-05-16-request-metadata-cow-zval-domain-review.md`: Blocker/High/Medium/Lowなし。

## 判断

- Status: Closed
- Decision: Adopted

metadata配列の所有権モデルはPHP COWに合っており、C側で書き込む箇所は `SEPARATE_ARRAY()` で分離した。互換性テストとreal Spanner比較で悪化が見えないため採用する。
