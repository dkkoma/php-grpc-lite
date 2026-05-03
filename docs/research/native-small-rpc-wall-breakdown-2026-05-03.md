# Native small RPC wall time breakdown (2026-05-03)

## 対象

Spanner 主用途に近い small unary / small server streaming で、`php-grpc-lite` の libcurl 経路、native nghttp2 PoC 経路、公式 `ext-grpc` を比較した。

- unary: `BeginTransaction` / DML `ExecuteSql` / `Commit` 相当の request/response bytes
- server streaming: `ExecuteStreamingSql` の 1 `PartialResultSet` 相当として、1 stream あたり 1 message
- 対向: Go test-server
- 実行: Docker compose

計測条件は warm を主比較にする。curl、native、`ext-grpc` は同一プロセス内で同じ client/stub を使い回し、計測前に warmup call/stream を実行する。cold は別の観測軸として扱い、warm の三者比較とは混ぜない。

## 原因

native unary の初期実装は、実 call surface で `nghttp2_poc_unary()` を毎回呼び、TCP connection と HTTP/2 session を call ごとに作成していた。

分解計測では、PHP 側の serialize / frame / deserialize / status build は支配的ではなかった。遅延はほぼ C transport call 内にあり、さらにその中では cold connection/session の固定費が支配していた。

| case | total p50 | native transport p50 | connect p50 | initial send p50 | recv loop p50 |
| --- | ---: | ---: | ---: | ---: | ---: |
| begin_txn | 136.4us | 130.5us | 60us | 39us | 10us |
| dml_insert_10col | 122.8us | 116.9us | 55us | 11us | 34us |
| dml_update_10col | 122.8us | 115.8us | 55us | 10us | 26us |
| dml_delete_10col | 129.2us | 122.9us | 63us | 9us | 26us |
| commit_txn | 149.8us | 141.6us | 71us | 11us | 42us |

curl 経路は Channel-scoped curl handle / connection reuse が効いており、同じ small unary では `curl_exec` p50 が 33-37us 程度だった。`ext-grpc` も channel/subchannel を再利用するため、native cold-per-call とは比較条件がずれていた。

## 対策

C PoC に最小の persistent channel API を追加した。

- `nghttp2_poc_channel_open(host, port)` で TCP connection と nghttp2 session を作成する
- `nghttp2_poc_channel_unary(channel, path, framed_request, headers)` で同一 HTTP/2 session 上に stream を追加する
- `NativeTransport::unarySimple()` は target ごとに channel を保持して再利用する
- channel resource は GOAWAY を受けたら `draining`、EOF/send/mem_recv error を受けたら `dead` にし、PHP 側の cache から外す
- 壊れた RPC は transport 層では自動 retry せず、次の RPC 開始時に新しい channel を作る
- server streaming の small response では `native_response_mode=simple` により同じ persistent simple 経路を使う

これは Phase 2 PoC の最小実装であり、TLS/mTLS、request をまたぐ FPM persistent resource、concurrent streams は未実装。

## 結果: small unary

`BENCH_TAG=20260503-spanner-dml-unary-persistent bench/phase2/compare-spanner-dml-unary-shape.sh`

| case | curl p50/p99 | native p50/p99 | ext-grpc p50/p99 |
| --- | ---: | ---: | ---: |
| begin_txn | 44.7 / 108.5us | 29.3 / 55.2us | 62.5 / 132.7us |
| dml_insert_10col | 40.2 / 121.8us | 27.1 / 88.0us | 58.2 / 87.5us |
| dml_update_10col | 37.3 / 57.9us | 26.2 / 70.7us | 64.8 / 867.4us |
| dml_delete_10col | 35.7 / 89.9us | 29.6 / 70.0us | 55.1 / 117.5us |
| commit_txn | 35.1 / 69.5us | 26.5 / 71.4us | 54.2 / 82.1us |

persistent native channel 適用後、small unary は p50 で curl / ext-grpc を上回った。p99 も概ね同等以上まで戻った。

適用後の wall time 分解では connect/setup が call path から消え、transport p50 は 24-32us 程度になった。

| case | total p50 | native transport p50 | connect p50 | initial send p50 | recv loop p50 |
| --- | ---: | ---: | ---: | ---: | ---: |
| begin_txn | 38.5us | 32.4us | 0us | 12us | 12us |
| dml_insert_10col | 37.7us | 31.6us | 0us | 23us | 4us |
| dml_update_10col | 29.4us | 25.0us | 0us | 6us | 16us |
| dml_delete_10col | 28.1us | 23.7us | 0us | 18us | 4us |
| commit_txn | 31.0us | 26.9us | 0us | 20us | 4us |

## 結果: small server streaming

`INCLUDE_POC=0 WARMUP_STREAMS=3 BENCH_TAG=20260503-small-select-warm3-smoke bench/phase2/compare-small-select-streaming.sh`

| case | curl p50/p99 | native-simple p50/p99 | ext-grpc p50/p99 |
| --- | ---: | ---: | ---: |
| 1x100B | 204.7 / 1523.6us | 34.8 / 334.4us | 75.3 / 810.7us |
| 1x1KiB | 202.7 / 1662.1us | 35.2 / 285.8us | 79.0 / 603.8us |
| 1x4KiB | 203.3 / 1724.0us | 38.2 / 370.0us | 81.4 / 396.3us |
| 1x10KiB | 208.5 / 1951.4us | 42.0 / 907.1us | 80.8 / 921.6us |

small server streaming も cold-per-stream が主因だった。persistent simple 経路では 1 message の SELECT 形状で curl / ext-grpc を上回った。

## 判断

small unary / small one-message server streaming に関しては、libcurl や protobuf decode ではなく、native PoC 側の connection/session reuse 欠落が主原因だった。

Phase 2 MVP の設計上は、native transport を採用するなら C 拡張内に Channel lifetime の HTTP/2 session を持つ必要がある。libcurl 経路を残す場合も、性能上の主経路ではなく安全・安定経路として扱うのが妥当。

残課題:

- GOAWAY / EOF / send error の再現テスト用 h2 server fixture
- TLS/mTLS channel
- FPM request をまたぐ persistent resource の可否
- concurrent streams / multiplex
- large streaming で `simple` と `compact64` / `direct` の選択ルールを再評価
