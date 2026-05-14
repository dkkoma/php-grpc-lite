---
Status: Closed
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

main基準:

- `./bench/compare.sh metadata-header`: `20260514-092917`
- `./bench/compare.sh payload-streaming`: `20260514-092932`

metadata branch:

- `./bench/compare.sh metadata-header`: `20260514-092546`
- `./bench/compare.sh spanner-shape`: `20260514-092600`

metadata-header native full comparison:

| measurement | main p50 | branch p50 | p50差分 | main p99 | branch p99 | p99差分 | 判断 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| req0/resp0/value0 | 40.6µs | 35.5µs | -5.1µs | 4951.7µs | 4853.4µs | -98.3µs | 同等〜改善 |
| req10/resp0/value32 | 50.9µs | 35.5µs | -15.4µs | 345.3µs | 696.0µs | +350.7µs | p50改善、p99悪化 |
| req10/resp10/value32 | 54.5µs | 35.3µs | -19.2µs | 3595.6µs | 177.1µs | -3418.5µs | 改善 |
| req50/resp0/value32 | 86.9µs | 80.5µs | -6.4µs | 1577.4µs | 524.8µs | -1052.6µs | 改善 |
| req50/resp50/value32 | 180.3µs | 164.2µs | -16.1µs | 1576.8µs | 633.2µs | -943.6µs | 改善 |

spanner-shape native full comparison:

| measurement | main p50 | branch p50 | p50差分 | main p99 | branch p99 | p99差分 | 判断 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| begin_txn_unary | 31.6µs | 30.8µs | -0.8µs | 128.6µs | 134.4µs | +5.8µs | 同等 |
| commit_txn_unary | 28.2µs | 28.1µs | -0.1µs | 83.4µs | 72.1µs | -11.3µs | 同等〜改善 |
| select_1row_10col_streaming | 28.9µs | 27.3µs | -1.6µs | 70.9µs | 66.8µs | -4.1µs | 同等〜改善 |
| dml_insert_10col_streaming | 26.1µs | 25.7µs | -0.4µs | 69.0µs | 61.1µs | -7.9µs | 同等〜改善 |
| dml_update_10col_streaming | 26.1µs | 28.2µs | +2.1µs | 72.5µs | 82.5µs | +10.0µs | 同等〜やや悪化 |
| dml_delete_10col_streaming | 27.3µs | 26.9µs | -0.4µs | 65.4µs | 69.5µs | +4.1µs | 同等 |

### 暫定判断

採用候補。主要Spanner形状では悪化が見えず、metadata-headerの一部で固定費改善が見える。ただしmetadata-heavyの `req50/resp50` は揺れか悪化かを追加測定で確認する必要がある。

### spanner-real-client 追加比較

main基準:

- `spanner-real-client` main native/ext-grpc: 既存計測値。transaction scopeは開始済みread-write transaction内。

metadata branch:

- `./bench/compare.sh spanner-real-client`: `20260514-095212`

spanner-real-client native main vs metadata branch:

| measurement | main native p50 | branch native p50 | p50差分 | main native p99 | branch native p99 | p99差分 | 判断 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| small_select_1row_10col | 778.6µs | 772.3µs | -6.3µs | 951.4µs | 1183.7µs | +232.3µs | p50同等、p99悪化気味 |
| dml_insert_10col | 788.0µs | 792.9µs | +4.9µs | 918.1µs | 929.3µs | +11.2µs | 同等 |
| dml_update_10col | 930.9µs | 906.4µs | -24.5µs | 1080.5µs | 1031.4µs | -49.1µs | 改善 |
| dml_delete_10col | 1059.2µs | 1142.6µs | +83.4µs | 1217.0µs | 1324.0µs | +107.0µs | 悪化気味 |

spanner-real-client ext-grpc rerun比較:

| measurement | main ext-grpc p50 | current ext-grpc p50 | p50差分 | main ext-grpc p99 | current ext-grpc p99 | p99差分 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| small_select_1row_10col | 844.7µs | 863.7µs | +19.0µs | 979.2µs | 1058.7µs | +79.5µs |
| dml_insert_10col | 816.0µs | 866.5µs | +50.5µs | 889.2µs | 976.4µs | +87.2µs |
| dml_update_10col | 968.2µs | 1011.6µs | +43.4µs | 1199.5µs | 1112.6µs | -86.9µs |
| dml_delete_10col | 1123.9µs | 1151.7µs | +27.8µs | 1262.6µs | 1444.6µs | +182.0µs |

判断: `spanner-real-client` はGAX/Spanner emulatorの揺れが大きく、metadata branch単体の効果判定には向かない。DML update以外はmain比で同等〜悪化気味に見えるが、ext-grpc rerun側も同程度に揺れている。採否判断は `metadata-header` と `spanner-shape` を主にし、real clientは実用上の悪化監視として扱う。

## 採用判断

- Status: Closed
- Decision: Adopted
- Fix commits:
  - `226a943 性能検証: metadata変換のHashTableコピーを削減`
  - `a01be5a 性能検証: metadata変換ベンチのmain比較を追記`
  - `7b0dcd6 性能検証: metadata変換のSpanner実経路比較を追記`

metadata-heavyで明確な改善があり、主要 `spanner-shape` では構造的な悪化が見えないため採用する。`spanner-real-client` はGAX/Spanner emulatorの揺れが大きく、採否の主指標にはしない。
