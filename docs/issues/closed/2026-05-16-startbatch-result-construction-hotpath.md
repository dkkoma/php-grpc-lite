---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# startBatch result constructionの空metadata/status固定費を削減する

## 目的

`Call::startBatch()` の返却object構築で、空metadataやstatus object周辺のzval allocation/copyを減らす。

## 背景

mixed transactionでは複数RPCが短時間に実行される。現行実装はAPI互換shapeを満たすため、空metadataでも配列を構築し、status objectにもmetadataを付ける。ext-grpcとのCPU差の一部がC/PHP bridge固定費にある可能性が高い。

## スコープ

- API shapeを変えずに、空metadata配列・status metadataの生成回数を減らせるか調査する。
- 可能ならstatic empty array相当、move semantics、copy回避を導入する。

## 非スコープ

- `Grpc\\Call::startBatch()` の公開戻り値shape変更。
- ext-grpc非互換の省略。

## 検証

- PHPT / PHPUnit。
- real Spanner mixed transaction c16。

## 完了条件

- 互換性を維持したまま固定費削減可否が判断される。
- 採用/見送り理由と計測値が残る。

## 実装試行

- `grpc_lite_add_event_metadata()` でmetadata配列を `zend_hash_copy()` するのをやめ、`add_property_zval()` によるPHP COW共有へ変更した。
- raw `Grpc\Call::startBatch()` のevent metadataをユーザーが変更しても、内部 `call->initial_metadata` が変更されないことをPHPTに追加した。

## 検証

- `make test TESTS="tests/010-unary.phpt tests/011-server-streaming.phpt tests/023-metadata-and-call-credentials.phpt"`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-c-static-analysis.sh`: PASS
- review: `docs/reviews/issues/2026-05-16-startbatch-metadata-cow-review.md`

## ベンチマーク

real Spanner Laravel/FPM, `transaction_select2_update1_insert1`, 256 requests, concurrency 16, 16 FPM workers, cloud Spanner.

| variant | cpu_us/req | rps | avg_ms | p50_ms | p90_ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| grpc-lite user-agent hotpath | 11454.7 | 25.4579 | 612.5 | 618.2 | 676.6 |
| grpc-lite StartBatch metadata COW | 11620.8 | 26.0591 | 603.4 | 610.0 | 685.1 |

metadata-header suite (`./bench/compare.sh metadata-header`, run id `20260516-091305`):

| measurement | php-grpc-lite p50 us | ext-grpc p50 us | note |
| --- | ---: | ---: | --- |
| req0_resp0_value0b | 98.9 | 104.7 | lite slightly faster p50 |
| req10_resp0_value32b | 106.8 | 106.0 | same |
| req10_resp10_value32b | 72.9 | 112.6 | lite faster p50 |
| req50_resp0_value32b | 153.2 | 161.9 | lite slightly faster p50 |
| req50_resp50_value32b | 231.0 | 246.7 | lite slightly faster p50 |

## 判断

- 採用しない。
- zval COWとしては安全だが、実ワークロードmixed transactionのCPU/requestは改善せず、むしろ悪化寄りだった。
- metadata-header microではp50が良いケースもあるが、今回のCPU差の主因ではない。公開結果objectのaliasingに見える設計リスクを取るほどの効果はない。
- 実装差分は破棄し、raw startBatch COW境界テストも未採用案に紐づくため取り込まない。
