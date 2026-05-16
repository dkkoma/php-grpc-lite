---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# persistent connection再利用時のcredential identity再検証をhot pathから外す

## 目的

persistent HTTP/2 connection reuse時に、毎RPCごとにroot certs / cert chain / private keyを再hash・memcmpする固定費を削減する。

## 背景

`Channel::__construct()` で `connection_key` を作るようにした後も、`get_persistent_connection()` は既存entryを見つけたあと `connection_entry_matches_identity()` を呼び、root certsなどのcredential bytesを毎回hash/memcmpしている。GAX/Spanner経路ではdefault roots PEMが大きいため、mixed transactionの複数RPCでこの固定費が残る可能性が高い。

## スコープ

- persistent connection entryに、reuse lookupに使った小さいconnection key identityを保持する。
- reuse時はentry keyと要求keyの一致を確認し、巨大credential bytesの再検証を行わない。
- connection作成時には従来どおりhost/authority/TLS materialからentryを作る。
- TLS/mTLS credential isolationの安全性をレビューする。

## 非スコープ

- persistent connection key formatの大規模変更。
- connection poolやmultiplex設計の変更。

## 検証

- TLS/mTLS PHPT。
- request metadata/control/lifecycle PHPT。
- real Spanner Laravel/FPM mixed transaction c16。
- domain model review。

## 完了条件

- 毎RPCの巨大credential revalidationが消える。
- credential isolation上の判断がレビュー記録に残る。
- CPU/request改善または非改善が記録される。

## 実装

- `Channel::__construct()` 時に作る `connection_key` を、length-prefixed canonical channel identity全体の `sha256:<64hex>` に変更した。
- credential PEM materialはkey生成時だけSHA-256 digest化し、persistent connection reuse時は巨大PEM bytesの再hash/memcmpを行わない。
- persistent connection entryには小さい `connection_key_identity` だけをpersistent zend_stringとして保持する。
- bench direct entrypointsもcaller supplied keyを信用せず、同じcanonical keyを内部生成して `get_persistent_connection()` へ渡すようにした。

## レビュー指摘と対応

- High: `|` delimiter joinは非injectiveで、full revalidation削除後に異なるidentityが同じkeyへaliasする可能性があった。
  - 対応: variable fieldをlength-prefixしたcanonical bytesを作り、その全体SHA-256 digestをconnection keyにした。
- Medium: 長いauthority/tls overrideで、internal key length validationによりunaryとserver streamingが分岐する可能性があった。
  - 対応: connection keyを固定長 `sha256:<64hex>` にした。
- Low: bench direct entrypointsが任意keyを受け取り、TLS materialと無関係なkey reuseが可能だった。
  - 対応: bench direct entrypointsでもTLS materialを含むcanonical keyを内部生成する。

## 検証

- `make test TESTS="tests/004-object-lifecycle.phpt tests/010-unary.phpt tests/011-server-streaming.phpt tests/024-control-semantics.phpt tests/030-tls.phpt"`: PASS
- `make test TESTS="tests/001-load.phpt tests/010-unary.phpt tests/011-server-streaming.phpt tests/030-tls.phpt"`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-c-static-analysis.sh`: PASS

## ベンチマーク

real Spanner Laravel/FPM, `transaction_select2_update1_insert1`, 256 requests, concurrency 16, 16 FPM workers, cloud Spanner.

| variant | cpu_us/req | rps | avg_ms | p50_ms | p90_ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| before root cause fix, channel key cache後 | 11431.5 | 25.2613 | 618.6 | 621.0 | 737.6 |
| identity key, pre-review fix | 9387.4 | 26.8616 | 584.2 | 590.9 | 661.9 |
| identity key, canonical digest | 9924.8 | 26.0319 | 599.8 | 607.9 | 678.3 |

同一run比較 (`cpu-gap-identity-key2-compare-mixed-c16-20260516`):

| variant | cpu_us/req | rps | avg_ms | p50_ms | p90_ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| grpc-lite canonical digest | 9215.4 | 25.9296 | 603.5 | 611.4 | 678.7 |
| ext-grpc 1.58 optimized | 11330.1 | 26.1425 | 603.3 | 612.2 | 679.7 |

## 判断

- 採用する。
- CPU/requestはrun間で揺れるが、既存の約11.4ms/reqから約9.2〜9.9ms/reqへ明確に改善している。
- ext-grpcとの比較でも、同一runではgrpc-liteがCPU/requestで優位になった。
- CPU差の最大要因は、persistent connection reuse時にdefault roots PEMを含むcredential materialを毎RPC再hash/revalidateしていたことだったと判断する。

## 追加修正

- bench buildで `grpc_lite_channel_close($key)` がcanonical key導入後に旧caller keyを参照してしまうため、未使用のdiagnostic close APIを削除した。
- bench buildで残っていた未定義 `bench_observe_message_ready()` 呼び出しを削除した。
- bench build確認: `./configure --enable-grpc --enable-grpc-bench && make`; `grpc_lite_unary` は存在し、`grpc_lite_channel_close` は存在しないことを確認。

## 最終検証

- bench build: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-c-static-analysis.sh`: PASS
