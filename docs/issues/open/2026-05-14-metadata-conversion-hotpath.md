---
Status: Open
Owner: Codex
Created: 2026-05-14
Branch: perf/metadata-conversion-hotpath
---

# metadata変換 hot path 最適化検証

## 目的

C拡張内の response metadata / status 変換固定費を下げられるかを、production互換性を保ったまま検証する。

## 背景

small unary / small server streaming ではpayload処理よりも固定費が支配的になりやすい。`on_header_callback()` はHTTP/2 header fieldごとに呼ばれ、gRPC必須header/status処理とPHP metadata配列向けの蓄積処理を同時に行っている。metadataが実際に参照されない通常経路では、app metadataのPHP向け構築を遅延または軽量化できる可能性がある。

## スコープ

- `ext/grpc/transport.c` の `on_header_callback()` 周辺を読む。
- `grpc-status` / `grpc-message` / `content-type` / `grpc-encoding` など protocol必須処理と、user metadata保存処理を分離して評価する。
- `getMetadata()` / `getTrailingMetadata()` / status取得時の互換性を壊さずに、通常のresponse受信時固定費を下げられるか試す。
- 変更前後で主要ベンチを計測し、採用可否を判断する。

## 非スコープ

- ext-grpc実装調査。
- GAX / cloud-spanner の上位層最適化。
- nghttp2置き換え。
- response body delivery の最適化。これは別issueで扱う。

## 仮説

- metadataを常にPHP配列として即時構築している場合、metadataを参照しないRPCでも固定費を払っている。
- protocol必須headerだけを即時処理し、app metadataは必要時構築または軽量なC表現に寄せることで、small RPCのp50/p99を改善できる可能性がある。
- ただしmetadata entry数が少ない通常ケースでは改善幅は限定的な可能性がある。

## 計画

1. 現在のmetadata保存・PHP配列化経路を分類する。
2. 互換性上即時処理が必要なheaderと遅延可能なmetadataを分ける。
3. 最小のproduction変更でhot path固定費を削る。
4. PHPT / PHPUnit / C coverage対象の互換テストを更新する。
5. `spanner-shape`、`spanner-real-client`、small unary/server streaming代表ケースを比較する。

## 採用判断

- 互換性テストが維持されること。
- metadataを参照しない主要ケースで悪化がないこと。
- p50/p99に測定可能な改善がある、またはコード構造が明確に改善すること。
- metadata heavy caseで許容不能な悪化がないこと。

## 検証予定

- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-c-coverage.sh`
- `docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit`
- `./bench/compare.sh spanner-shape`
- `./bench/compare.sh spanner-real-client`

## 判断ログ

- このissueは response delivery 最適化とは分ける。metadata変更だけで採否判断できるbranchにする。

## 2026-05-14 検証メモ

### 実装した候補

- server streamingでinitial metadataをmessageごとに再構築しないようにした。最初のmessageでのみmetadata mapを作り、それ以降のmessage resultは空配列にする。
- unary resultから `Grpc\Call` objectへmetadataを移す際、HashTable copyではなくownership moveにした。
- status objectの `metadata` は明示的なHashTable copyではなくCOW参照で設定するようにした。PHP側でmutationされてもCOWで分離される前提。

### 検証

- PHPT: pass。15/15。
- C unit: pass。protocol/status/transport core。
- `./bench/compare.sh metadata-header`: `20260514-092546`。
- `./bench/compare.sh spanner-shape`: `20260514-092600`。

### ベンチ結果

metadata-header native p50:

| measurement | before-ish p50 | after p50 | 判断 |
| --- | ---: | ---: | --- |
| req0/resp0/value0 | 33.2µs | 35.5µs | 同等、ノイズ範囲 |
| req10/resp0/value32 | 39.5µs | 35.5µs | 改善 |
| req10/resp10/value32 | 49.0µs | 35.3µs | 改善 |
| req50/resp0/value32 | 93.4µs | 80.5µs | 改善 |
| req50/resp50/value32 | 149.5µs | 164.2µs | 悪化気味、再測定余地あり |

spanner-shape native p50/p99:

| measurement | p50 | p99 | 判断 |
| --- | ---: | ---: | --- |
| begin_txn_unary | 30.8µs | 134.4µs | 同等 |
| commit_txn_unary | 28.1µs | 72.1µs | 同等 |
| select_1row_10col_streaming | 27.3µs | 66.8µs | 同等〜改善 |
| dml_insert_10col_streaming | 25.7µs | 61.1µs | 同等〜改善 |
| dml_update_10col_streaming | 28.2µs | 82.5µs | 同等 |
| dml_delete_10col_streaming | 26.9µs | 69.5µs | 同等 |

### 暫定判断

採用候補。主要Spanner形状では悪化が見えず、metadata-headerの一部で固定費改善が見える。ただしmetadata-heavyの `req50/resp50` は揺れか悪化かを追加測定で確認する必要がある。
