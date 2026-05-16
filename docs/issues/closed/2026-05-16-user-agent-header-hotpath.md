---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# user-agentをPHP metadata配列経由ではなくHTTP/2 header buildで直接付与する

## 目的

RPCごとのuser-agent付与で発生するPHP zval配列構築・metadata merge・header変換の固定費を削減する。

## 背景

現行実装は `grpc_lite_append_user_agent()` で `call->metadata` に `user-agent => [value]` を追加し、その後 `append_custom_request_headers()` がmetadata配列を走査してHTTP/2 headerへ変換する。user-agentはwire headerであり、アプリケーションmetadataとして保持する必要は薄い。

## スコープ

- channelのprimary/default user-agentをrequest header build時に直接追加する。
- ユーザー指定metadataとの禁止key/filtering semanticsを維持する。
- `user-agent` がアプリケーションmetadataとして漏れないことを維持する。

## 非スコープ

- user-agent値そのものの変更。
- call credentials metadataの仕様変更。

## 検証

- request metadata validation/filtering PHPT。
- metadata compatibility PHPUnit。
- real Spanner mixed transaction c16。

## 完了条件

- `grpc_lite_append_user_agent()` のPHP metadata配列固定費がhot pathから消える。
- CPU/request改善または非改善が記録される。

## 実装

- native HTTP/2 transportでは `Grpc\Call` のPHP metadata配列へ `user-agent` を追加せず、request header build時に直接 `user-agent` を追加するようにした。
- ユーザー指定の `user-agent` はcustom metadataとしては送信せず、固定のwire headerを優先する既存仕様を維持した。
- franken-go backendはPHP backend APIへmetadataを渡す必要があるため、従来どおりmetadata配列に `user-agent` を含める。

## 検証

- `make test TESTS="tests/020-request-metadata-control.phpt tests/023-metadata-and-call-credentials.phpt tests/026-franken-go-backend.phpt tests/010-unary.phpt tests/011-server-streaming.phpt"`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-c-static-analysis.sh`: PASS
- domain self-review: `docs/reviews/issues/2026-05-16-user-agent-hotpath-domain-self-review.md`

## ベンチマーク

条件: real Spanner Laravel/FPM, `transaction_select2_update1_insert1`, 256 requests, concurrency 16, 16 FPM workers, cloud Spanner.

| variant | cpu_us/req | rps | avg_ms | p50_ms | p90_ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| grpc-lite after channel key cache | 11431.5 | 25.2613 | 618.6 | 621.0 | 737.6 |
| grpc-lite user-agent hotpath | 11454.7 | 25.4579 | 612.5 | 618.2 | 676.6 |

## 判断

- CPU/requestは明確には改善していないため、性能改善効果はノイズ範囲。
- ただしnative HTTP/2のwire header責務をtransport側へ寄せ、PHP metadata配列の意味をアプリケーションmetadataへ戻す設計整理として採用する。
- ext-grpcとの差分を埋める主因ではない。
