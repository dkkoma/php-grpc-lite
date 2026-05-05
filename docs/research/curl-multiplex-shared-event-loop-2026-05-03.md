# curl_multi PoC: shared event loop / HTTP/2 multiplex (2026-05-03)

## 目的

ext-grpc/C-coreに近い構造として、call単位のblocking loopではなく、channel transportが複数streamを同一event loopで進めると何が変わるかを観測する。

HTTP/2 transportへ直接入る前に、まず既存ext-curlでできる範囲として以下を試した。

- 1つの `curl_multi` event loop。
- HTTP/2 prior knowledge。
- `CURLMOPT_PIPELINING = CURLPIPE_MULTIPLEX`。
- `CURLMOPT_MAX_HOST_CONNECTIONS = 1`。
- 複数 `BenchServerStream` callを同時に追加。

これはlibcurl経由のPoCであり、nghttp2実装ではない。またprotobuf decode/yieldは行わず、gRPC frameのmessage countだけを数える。目的はAPI実装候補ではなく、shared event loop / multiplexの性質を見ること。

## 実装

追加:

- `tools/phase2/curl-multiplex-streaming.php`
- `bench/phase2/compare-curl-multiplex-event-loop.sh`

計測する値:

- total wall time
- per-stream p50 / p99
- messages/sec
- streams/sec
- `curl_local_ports_unique`

`curl_num_connects_total` は今回の環境では常に0として返ったため判断に使わない。`curl_local_ports_unique=1` なので、少なくとも同一local portのconnectionを共有している。

## 再実行方法

```bash
BENCH_TAG=20260503-curl-multiplex-event-loop bench/phase2/compare-curl-multiplex-event-loop.sh
```

出力:

- summary: `var/bench-results/phase2-curl-multiplex-event-loop-20260503-curl-multiplex-event-loop.tsv`
- per-run JSON: `var/bench-results/phase2-curl-multiplex-event-loop-20260503-curl-multiplex-event-loop-*.json`

## 結果

| case | concurrency | wall | p50 stream | p99 stream | msg/s | streams/s | local ports |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1000×100B | 1 | 171.3ms | 741.4μs | 2237.4μs | 1167409.5 | 1167.4 | 1 |
| 1000×100B | 2 | 163.8ms | 1493.2μs | 3609.9μs | 1221225.5 | 1221.2 | 1 |
| 1000×100B | 4 | 164.5ms | 3230.6μs | 6711.3μs | 1215898.1 | 1215.9 | 1 |
| 1000×100B | 8 | 156.7ms | 6242.0μs | 10604.5μs | 1276403.2 | 1276.4 | 1 |
| 10×100KiB | 1 | 239.4ms | 564.6μs | 2712.4μs | 12532.8 | 1253.3 | 1 |
| 10×100KiB | 2 | 229.8ms | 1165.1μs | 5334.4μs | 13057.0 | 1305.7 | 1 |
| 10×100KiB | 4 | 146.0ms | 1708.0μs | 4941.4μs | 20548.9 | 2054.9 | 1 |
| 10×100KiB | 8 | 120.9ms | 2719.5μs | 9433.0μs | 24814.6 | 2481.5 | 1 |
| 1×1MiB | 1 | 256.5ms | 540.8μs | 4590.1μs | 1169.4 | 1169.4 | 1 |
| 1×1MiB | 2 | 237.8ms | 1165.5μs | 7064.9μs | 1261.5 | 1261.5 | 1 |
| 1×1MiB | 4 | 193.6ms | 2055.6μs | 7923.5μs | 1549.7 | 1549.7 | 1 |
| 1×1MiB | 8 | 164.0ms | 3816.7μs | 11947.4μs | 1829.7 | 1829.7 | 1 |

## 判断

shared event loop / HTTP/2 multiplexは、**複数callをまとめたthroughputには効く**。

- `10×100KiB`: concurrency 1 → 8 で msg/s は `12532.8 -> 24814.6`。
- `1×1MiB`: concurrency 1 → 8 で msg/s は `1169.4 -> 1829.7`。
- `1000×100B`: 改善は小さいが、concurrency 8で少し上がる。

一方で、**個々のstream latencyは悪化する**。

- `1000×100B`: p99 `2237.4μs -> 10604.5μs`
- `10×100KiB`: p99 `2712.4μs -> 9433.0μs`
- `1×1MiB`: p99 `4590.1μs -> 11947.4μs`

これは単純な改善候補ではなく、workload依存のtransport設計要素として扱うべき。

## HTTP/2 transportへの含意

HTTP/2 transportでchannel shared event loopを入れるなら、目的は単一call latency削減ではなく、以下になる。

1. FPM worker内で複数gRPC callが同時に走る場合のconnection/session共有。
2. 複数streamのHTTP/2 flow-control/window updateを1つのtransport loopで進めること。
3. connection確立とHTTP/2 session管理の固定費をchannel lifetimeへ寄せること。
4. multiplex時のper-stream tail悪化を避けるfair scheduler / backpressure。

今回のPoCだけでは、Phase2の最優先をshared event loopに変更する根拠はない。

優先度は以下。

1. unary large request / server streaming large responseで確認済みのHTTP/2 transport pathを本実装設計へ落とす。
2. response pathはcompact/ring buffer + direct payload assemblyを基本にする。
3. shared event loop/channel transportは、concurrent workload向けの次段階PoCとして扱う。

## 未解決

このPoCはlibcurl上のmultiplexであり、nghttp2で複数active streamを直接管理したわけではない。

次にさらに進めるなら、C PoC側で以下を作る必要がある。

- 複数stream stateを `poc_client` から分離する。
- `stream_id -> stream state` のmapを持つ。
- DATA/header/trailer callbackをstream stateへdispatchする。
- 1つの `nghttp2_session` と1つのpoll loopで複数streamをcloseまで進める。

ただしこれは実装規模が大きく、現時点では「Phase2判断の必須追加材料」ではなく、concurrency最適化の別テーマとして扱うのが妥当。
