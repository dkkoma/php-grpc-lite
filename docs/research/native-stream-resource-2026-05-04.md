# Native Stream Resource 実装と主要ベンチ再取得 (2026-05-04)

## 目的

server streaming HTTP/2 surfaceを `unaryBatch()` のbatch drain後yieldから、C stream resourceをPHP Generatorがpullする形へ切り替える。

狙い:

- messageごとに `ServerStreamingCall::responses()` からyieldする。
- slow consumer時は次message要求まで追加read / WINDOW_UPDATE進行を止める。
- `cancel()` でactive streamに `RST_STREAM(CANCEL)` を送る。
- HTTP/2 session/socketはC側persistent channelに保持し、PHP userlandではchannelを保持しない。

## 実装

- `nghttp2_poc_stream_open()`
  - C側persistent channel cacheからchannelを取得する。
  - 1 session内1 active stream前提でstream resourceを作成する。
  - gRPC 5B header + request payloadはnghttp2 data providerで送る。
- `nghttp2_poc_stream_next()`
  - 次のcomplete gRPC messageまでread / nghttp2 processingを進める。
  - messageができたらpayloadだけ返す。
  - stream完了時はstatus / timing trailer相当のraw metadataを返す。
- `nghttp2_poc_stream_cancel()`
  - active streamへ `RST_STREAM(CANCEL)` を送る。
- `ServerStreamingCall::responses()`
  - native経路ではstream resourceを開き、Generatorのpullに合わせて `streamNext()` を呼ぶ。

## 検証

```bash
docker compose run --rm dev sh -lc 'cd poc/nghttp2-client-ext && make -j2'
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so vendor/bin/phpunit tests/Integration/Http2TransportControlTest.php
docker compose run --rm dev vendor/bin/phpunit
```

結果:

- `Http2TransportControlTest`: 13 tests / 38 assertions / 1 skipped
- 全体PHPUnit: 47 tests / 125 assertions / 11 skipped

## Small SELECT Streaming

実行:

```bash
BENCH_TAG=20260504-native-stream-resource INCLUDE_POC=0 NATIVE_RESPONSE_MODE=stream WARMUP_STREAMS=3 bench/phase2/compare-small-select-streaming.sh
```

summary: `var/bench-results/phase2-small-select-streaming-20260504-native-stream-resource.tsv`

| case | curl p50/p99 | native p50/p99 | ext-grpc p50/p99 | 見解 |
|---|---:|---:|---:|---|
| 1x100B | 204.4 / 1564.0 μs | 37.4 / 257.6 μs | 74.8 / 430.3 μs | nativeが最良 |
| 1x1KiB | 208.2 / 1796.9 μs | 39.2 / 312.3 μs | 82.7 / 729.5 μs | nativeが最良 |
| 1x4KiB | 209.6 / 1863.1 μs | 40.4 / 423.1 μs | 88.4 / 789.1 μs | nativeが最良 |
| 1x10KiB | 208.1 / 1581.2 μs | 41.5 / 721.7 μs | 89.4 / 926.8 μs | nativeが最良 |

small SELECT代表形状では、native stream resource化によりcurlより大幅に速く、ext-grpcよりp50/p99とも良い。

## Spanner DML Unary Shape

実行:

```bash
BENCH_TAG=20260504-native-stream-resource DURATION=1 WARMUP_CALLS=3 bench/phase2/compare-spanner-dml-unary-shape.sh
```

summary: `var/bench-results/phase2-spanner-dml-unary-shape-20260504-native-stream-resource.tsv`

| case | curl p50/p99 | native p50/p99 | ext-grpc p50/p99 | 見解 |
|---|---:|---:|---:|---|
| begin_txn | 37.5 / 83.5 μs | 28.5 / 65.8 μs | 56.4 / 115.2 μs | nativeが最良 |
| dml_insert_10col | 37.1 / 81.8 μs | 28.7 / 65.6 μs | 56.6 / 100.4 μs | nativeが最良 |
| dml_update_10col | 37.3 / 79.3 μs | 28.8 / 66.7 μs | 53.8 / 97.6 μs | nativeが最良 |
| dml_delete_10col | 38.1 / 81.5 μs | 28.3 / 67.7 μs | 45.6 / 96.6 μs | nativeが最良 |
| commit_txn | 38.3 / 80.7 μs | 28.7 / 67.5 μs | 55.1 / 101.4 μs | nativeが最良 |

## Phase2 Main Comparison

実行:

```bash
BENCH_TAG=20260504-native-stream-resource-main bench/phase2/compare-native-mvp-vs-libcurl-ext.sh
```

summary: `var/bench-results/phase2-native-mvp-vs-libcurl-ext-20260504-native-stream-resource-main.tsv`

主要surface結果:

| case | curl p50/p99 | native p50/p99 | ext-grpc p50/p99 | 見解 |
|---|---:|---:|---:|---|
| large request unary 1MiB | 843.8 / 4182.4 μs | 681.4 / 4240.7 μs | 441.3 / 3946.5 μs | ext-grpcが最良、nativeはcurlよりp50改善 |
| 1000x100B stream | 4815.6 / 9731.3 μs | 4207.9 / 8851.3 μs | 5265.0 / 9315.3 μs | nativeが最良 |
| 10x100KiB stream | 910.3 / 3889.4 μs | 797.1 / 4214.7 μs | 622.3 / 3362.7 μs | ext-grpcが最良 |
| 100x100KiB stream | 5324.9 / 11016.7 μs | 9472.6 / 15326.9 μs | 5408.6 / 9540.7 μs | HTTP/2 surfaceが悪化、要調査 |
| 1x1MiB stream | 893.5 / 4281.0 μs | 733.5 / 4017.9 μs | 546.2 / 3422.2 μs | ext-grpcが最良、nativeはcurlより改善 |
| 10000x100B stream | 46222.7 / 56707.3 μs | 40027.5 / 54340.1 μs | 43551.6 / 49963.8 μs | nativeはp50/throughput良、p99はext-grpc良 |

## 判断

- small unary / small SELECT streamingという主ワークロードではnativeがcurl/ext-grpcより優位になった。
- production server streaming resource化により、batch drain後yieldのrelease blockerは解消方向に進んだ。
- large / many-large responseでは、PoC direct/compactに比べてHTTP/2 surfaceがまだ遅いケースがある。特に `100x100KiB` はstream resourceのpull粒度、queued payload、PHP deserialize/yield、WINDOW_UPDATE進行のどこで差が出ているかを追加分解する必要がある。
- Phase2の次タスクは「large streaming surfaceのPoC差分を潰す」か、「互換性ゲートのnative照合」に進めるのが妥当。
