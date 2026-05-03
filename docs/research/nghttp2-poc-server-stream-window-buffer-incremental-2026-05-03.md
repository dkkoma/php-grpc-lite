# nghttp2 PoC: server streaming window/buffer tuning and incremental delivery (2026-05-03)

## 目的

server streaming call について、以下を確認する。

1. receive window default 値の最善パターン。
2. receive buffer size の最善パターン。
3. DATA 受信中に complete gRPC frame を逐次 decode/yield する delivery strategy の影響。

## 固定条件

- nghttp2 PoC
- server streaming
- no-copy request path
- poll loop
- `flush-after-mem-recv`
- protobuf decode + 1-message generator yield
- Go test-server default settings

window/buffer sweep では以下を組み合わせた。

- receive window: 1MiB / 4MiB / 8MiB / 16MiB
- receive buffer: 16KiB / 32KiB / 64KiB / 128KiB / 256KiB

ケース:

- `stream_1000x100b`
- `stream_10x100k`
- `stream_1x1m`

## window × buffer sweep

各ケースで p99 が最小だった組み合わせ。

| case | best window | best buffer | p50 | p99 | throughput | server last OutPayload p99 | decode/yield p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1000×100B | 8MiB | 32KiB | 4731.0μs | 7250.0μs | 208420.2 msg/s | 6726.2μs | 324.0μs |
| 10×100KiB | 8MiB | 64KiB | 545.0μs | 3355.0μs | 11318.4 msg/s | 3154.5μs | 33.0μs |
| 1×1MiB | 16MiB | 64KiB | 503.0μs | 3621.0μs | 1373.4 msg/s | 2862.0μs | 31.0μs |

判断:

- 単一の万能 default は見えない。
- 8MiB window は `1000×100B` と `10×100KiB` の best で、バランスが良い。
- 1×1MiB の p99 best は 16MiB window + 64KiB buffer。
- receive buffer は 64KiB が large response では安定しやすい。256KiB はこの sweep でも best ではなかった。

## incremental decode/yield

PoC に `--incremental-decode` を追加し、`on_data_chunk_recv_callback()` 内で complete gRPC frame を見つけた時点で PHP callback を呼ぶようにした。

比較したパターン:

| pattern | window | buffer | 意図 |
| --- | ---: | ---: | --- |
| balanced | 8MiB | 16KiB | 保守的なbufferでwindowだけ広げる |
| small_best | 8MiB | 32KiB | many-small streaming重視 |
| multi_large_best | 8MiB | 64KiB | 10×100KiB重視 |
| large_best | 16MiB | 64KiB | 1×1MiB重視 |

結果:

| pattern | case | p50 | p99 | throughput | server last OutPayload p99 | decode/yield p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| balanced | 1000×100B | 4329.0μs | 8337.0μs | 218509.0 msg/s | 8268.6μs | 417.0μs |
| balanced | 10×100KiB | 518.0μs | 3755.0μs | 11979.3 msg/s | 3453.0μs | 43.0μs |
| balanced | 1×1MiB | 533.0μs | 4261.0μs | 1220.6 msg/s | 3608.3μs | 29.0μs |
| small_best | 1000×100B | 4388.0μs | 8218.0μs | 220564.3 msg/s | 8156.4μs | 361.0μs |
| small_best | 10×100KiB | 489.0μs | 3394.0μs | 12416.3 msg/s | 3323.6μs | 49.0μs |
| small_best | 1×1MiB | 513.0μs | 4184.0μs | 1244.2 msg/s | 3837.3μs | 29.0μs |
| multi_large_best | 1000×100B | 4520.0μs | 7878.0μs | 215301.2 msg/s | 7630.1μs | 377.0μs |
| multi_large_best | 10×100KiB | 507.0μs | 3762.0μs | 11750.5 msg/s | 3652.3μs | 35.0μs |
| multi_large_best | 1×1MiB | 489.0μs | 4189.0μs | 1261.5 msg/s | 3578.5μs | 29.0μs |
| large_best | 1000×100B | 4443.0μs | 7349.0μs | 219642.2 msg/s | 7217.6μs | 1036.0μs |
| large_best | 10×100KiB | 511.0μs | 3987.0μs | 11790.4 msg/s | 3926.6μs | 36.0μs |
| large_best | 1×1MiB | 501.0μs | 4088.0μs | 1232.1 msg/s | 3522.0μs | 29.0μs |

判断:

- 逐次 decode/yield は本物の streaming semantics には必要だが、p99 改善としては万能ではない。
- many-small streaming では `large_best` が p99 最良だが、decode/yield p99 が 1036μs まで伸びたため安定性に疑問がある。
- 10×100KiB では `small_best` が p99 最良。
- 1×1MiB では今回の逐次候補群はいずれも end-of-stream decode の best より遅い。

## ext-grpc / php-grpc-lite 比較

同じ3ケースを `streaming-diagnostic` で再取得した。

| case | implementation | p50 | p99 | throughput | server last OutPayload p99 |
| --- | --- | ---: | ---: | ---: | ---: |
| 1000×100B | php-grpc-lite | 4807.6μs | 10355.9μs | 194313.1 msg/s | 8206.1μs |
| 1000×100B | ext-grpc | 5055.7μs | 8361.7μs | 190530.6 msg/s | 7440.3μs |
| 10×100KiB | php-grpc-lite | 894.8μs | 3787.3μs | 8581.7 msg/s | 3018.1μs |
| 10×100KiB | ext-grpc | 630.5μs | 3525.0μs | 11204.5 msg/s | 3202.2μs |
| 1×1MiB | php-grpc-lite | 943.6μs | 4947.0μs | 775.7 msg/s | 4116.8μs |
| 1×1MiB | ext-grpc | 549.3μs | 3318.4μs | 1289.3 msg/s | 2590.5μs |

PoC の位置づけ:

- `1000×100B`: incremental PoC best p99 7349.0μs は ext-grpc p99 8361.7μs より良い。
- `10×100KiB`: incremental PoC best p99 3394.0μs は ext-grpc p99 3525.0μs と同等以上。
- `1×1MiB`: end-of-stream decode の sweep best p99 3621.0μs は ext-grpc p99 3318.4μs より遅い。逐次候補ではさらに遅い。

## 結論

### default候補

現時点での conservative default 候補は以下。

- receive window: 8MiB
- receive buffer: 32KiB or 64KiB
- `flush-after-mem-recv`: enabled
- incremental decode/yield: actual streaming API では enabled

ただし、buffer は固定値としてはまだ決めきれない。

- 32KiB: many-small と 10×100KiB のバランスが良い。
- 64KiB: large response寄りで強いが、many-small decode/yield p99 が荒れる可能性がある。
- 256KiB: 前回確認どおり default 固定は避ける。

### 逐次decode/yield

実装方針として逐次deliveryは必要。ただし、性能上は stream完了後まとめdecodeより良いとは限らない。

本実装では逐次deliveryを採りつつ、以下を追加で詰める必要がある。

1. 完全な gRPC frame が揃ったら即 decode するが、PHP callback/yield の粒度を過剰に細かくしない。
2. many-small streaming で decode/yield p99 が伸びる条件を再確認する。
3. body buffer を蓄積し続けず、consumed bytes をcompactまたはring buffer化する。

### 残課題

1×1MiB large response では ext-grpc にまだ届いていない。server last OutPayload p99 との差も残るため、次に掘るなら以下。

- received body buffer の蓄積をやめる compact/ring buffer。
- incremental decode 後の consumed bytes 解放。
- 1×1MiB に特化した window / buffer / flush policy の再確認。
