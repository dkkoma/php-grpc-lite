---
Status: Open
Owner: Codex
Created: 2026-05-14
Branch: perf/response-delivery-hotpath
---

# response delivery hot path 最適化検証

## 目的

C拡張内の response DATA 受信からPHP wrapperへmessageを返すまでの固定費を下げられるかを検証する。

## 背景

server streamingでは `Grpc\ServerStreamingCall::responses()` が `startBatch(OP_RECV_MESSAGE)` をmessageごとに呼ぶ。C拡張側では `on_data_chunk_recv_callback()` がDATA chunkを受け取り、direct decode / queue / `smart_str` append のいずれかに流す。small streamingではpayloadよりもmessage delivery境界、large responseではchunk処理・copy・queue戦略が効く可能性がある。

## スコープ

- `ext/grpc/transport.c` の `on_data_chunk_recv_callback()`、response message assembly、queue/read-ahead周辺を読む。
- unaryとserver streamingのresponse delivery経路を分けて評価する。
- `startBatch(OP_RECV_MESSAGE)` 固定費、message queue、read-ahead、payload copyの削減余地を検証する。
- 変更前後で主要ベンチを計測し、採用可否を判断する。

## 非スコープ

- ext-grpc実装調査。
- GAX / cloud-spanner の上位層最適化。
- nghttp2置き換え。
- metadata変換の最適化。これは別issueで扱う。

## 仮説

- server streamingの1 messageごとの `startBatch(OP_RECV_MESSAGE)` 境界に固定費がある。
- C側でmessageを自然にqueue/read-aheadし、PHP側では取り出しだけに寄せるとsmall streamingの固定費を下げられる可能性がある。
- large responseではchunk append/copy戦略がp99に効く可能性がある。
- ただしread-aheadはslow consumer時のmemory upper boundとトレードオフになる。

## 計画

1. 現在のDATA chunk受信、5B gRPC framing、message assembly、queue、PHP返却経路を分類する。
2. unary / server streamingで必要な処理を分け、共通化による余計な分岐がないか確認する。
3. read-ahead / queue / direct payload delivery の候補を1つずつ実装し、各checkpointでベンチを取る。
4. slow consumer / memory upper bound の回帰テストを確認する。
5. `spanner-shape`、`spanner-real-client`、large response代表ケースを比較する。

## 採用判断

- protocol互換性と既存テストを維持すること。
- small server streamingで悪化がないこと。
- large responseのp99改善、またはserver streaming fixed cost改善が確認できること。
- slow consumerでmemory上限が説明可能であること。

## 検証予定

- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-c-coverage.sh`
- `docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit`
- `./bench/compare.sh spanner-shape`
- `./bench/compare.sh spanner-real-client`
- large response / server streaming代表suite

## 判断ログ

- このissueは metadata変換最適化とは分ける。response delivery変更だけで採否判断できるbranchにする。
