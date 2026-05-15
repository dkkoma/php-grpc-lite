---
Status: Closed
Owner: Codex
Created: 2026-05-15
Branch: main
---

# request header allocation hot path を削減する

## 目的

毎RPCで `h2_request_headers` が `nghttp2_nv` / `name_strings` / `value_strings` をheap allocationする固定費を削減する。

## 背景

`init_request_headers()` は `count_custom_header_values()` の結果を使い、毎RPCで3つの配列を `ecalloc` している。Spanner mixed transactionのように1 HTTP request内で複数RPCを実行する経路では、この固定費がRPC数分積み上がる。

## スコープ

- 少数metadataの通常RPCでheap allocationを避ける。
- capacity超過時は従来通りheapへ拡張する。
- unary / server streaming のrequest header buildに適用する。

## 非スコープ

- HTTP/2 header semanticsの変更。
- request metadata validation仕様の変更。

## 計画

1. `h2_request_headers` に小さいinline bufferを持たせる。
2. `append_request_header()` をcapacity不足時にgrowできる形へ変更する。
3. 既存テスト、CPU micro、mixed transactionで効果を確認する。

## 進捗

- `h2_request_headers` に16要素のinline bufferを追加した。
- 通常の必須header 6〜7個と少数custom metadataでは、`nghttp2_nv` / `name_strings` / `value_strings` の3配列をheap allocationしない。
- inline capacityを超える場合だけheap bufferへgrowする。
- `free_request_headers()` はinline bufferを解放せず、heapへgrowした場合だけ `efree()` する。

## 検証

- Build: `docker compose run --rm dev sh -lc 'cd /workspace/ext/grpc && make clean >/dev/null 2>&1 || true && phpize >/dev/null && ./configure >/dev/null && make -j$(nproc)'`
- PHPT: `./tools/test/check-phpt.sh ext/grpc/tests/010-unary.phpt ext/grpc/tests/020-request-metadata-control.phpt ext/grpc/tests/022-error-and-http-validation.phpt ext/grpc/tests/023-metadata-and-call-credentials.phpt ext/grpc/tests/025-resource-limits.phpt`。preflight/default含め15 tests PASS。
- Review fix: `020-request-metadata-control.phpt` に24 value custom metadataをunary/server streaming双方でround-tripするgrow境界テストを追加し、`./tools/test/check-phpt.sh ext/grpc/tests/020-request-metadata-control.phpt` が15 tests PASS。
- CPU micro after header inline/grow:

| case | native CPU/call | note |
| --- | ---: | --- |
| `small_unary_100b` | 13.5µs | prior観測12.7µs近辺に対して小幅悪化/揺れ |
| `new_client_unary_100b` | 12.6µs | prior観測13.0µs近辺に対して小幅改善 |
| `begin_txn_unary` | 11.9µs | prior観測11.5µs近辺に対して同等 |
| `commit_txn_unary` | 11.6µs | prior観測11.7µs近辺に対して同等 |
| `select_1row_10col_streaming` | 12.7µs | prior観測12.5µs近辺に対して同等 |
| `dml_insert_10col_streaming` | 12.4µs | prior観測12.7µs近辺に対して小幅改善 |

- Metadata header comparison:

| case | php-grpc-lite p50 | ext-grpc p50 | note |
| --- | ---: | ---: | --- |
| `metadata_header_req_10_resp_0_value_32b` | 69.3µs | 119.2µs | metadata-heavy p50では優位 |
| `metadata_header_req_50_resp_0_value_32b` | 136.1µs | 147.0µs | metadata-heavy p50では小幅優位 |
| `metadata_header_req_50_resp_50_value_32b` | 192.7µs | 283.6µs | response metadata込みでもp50優位 |

## 判断

- 通常small RPCのCPU/call改善は明確ではない。効果は揺れ幅程度。
- ただし、heap allocationを通常経路から外す構造改善であり、metadata-heavy p50では悪化していない。
- 互換性・メモリ所有のリスクはPHPTとレビューで確認する前提で採用する。

## 完了条件

- 通常の7ヘッダ + 少数custom metadataでheader配列のheap allocationが不要になる。
- 互換性テストが通る。

## 完了

- 完了条件を満たしたためClosed。
- 修正コミット: this commit
