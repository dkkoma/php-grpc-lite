# Native slow consumer / memory surface (2026-05-04)

## 目的

server streaming native stream resourceが、consumerが遅い条件で無制限に先読みせず、user-visible latencyとmemoryがconsumer speedに従うことを確認する。

これはthroughput競争ではない。`responses()` のconsumer側に `usleep()` を入れ、transportが先に全responseをdrainしてしまう構造になっていないかを見る。

## Runner

```bash
STREAMS=5 MESSAGE_COUNT=100 PAYLOAD_BYTES=102400 SLEEP_US=1000 BENCH_TAG=20260504-slow-consumer-100x100k-rss bench/check-native-slow-consumer.sh
```

summary: `var/bench-results/phase2-slow-consumer-surface-20260504-slow-consumer-100x100k-rss.tsv`

比較対象:

- `php-grpc-lite` libcurl transport
- `php-grpc-lite` native stream resource
- official `ext-grpc`

## 100x100KiB / 1ms sleep

| metric | curl | native stream | ext-grpc |
|---|---:|---:|---:|
| wall ms | 988.5 | 985.3 | 975.4 |
| first yield p50 | 1261.3 μs | 1228.0 μs | 1585.9 μs |
| stream p50 | 197517.3 μs | 201289.3 μs | 194358.3 μs |
| stream p99 | 202483.7 μs | 207961.4 μs | 201041.6 μs |
| messages/s | 505.8 | 507.5 | 512.6 |
| PHP memory max | 1811192 bytes | 1998824 bytes | 1032456 bytes |
| PHP real memory max | 4194304 bytes | 4194304 bytes | 2097152 bytes |
| RSS max | 26336 KiB | 26124 KiB | 53280 KiB |
| RSS max delta | 1160 KiB | 900 KiB | 21112 KiB |

## Smoke

```bash
STREAMS=2 MESSAGE_COUNT=5 PAYLOAD_BYTES=1024 SLEEP_US=1000 BENCH_TAG=20260504-slow-consumer-smoke3 bench/check-native-slow-consumer.sh
```

summary: `var/bench-results/phase2-slow-consumer-surface-20260504-slow-consumer-smoke3.tsv`

| metric | curl | native stream | ext-grpc |
|---|---:|---:|---:|
| wall ms | 22.7 | 20.4 | 25.7 |
| first yield p50 | 791.5 μs | 200.1 μs | 160.2 μs |
| stream p99 | 11711.3 μs | 10018.6 μs | 12136.3 μs |
| RSS max | 25952 KiB | 25704 KiB | 36292 KiB |

## 判断

- `100x100KiB / 1ms sleep` では3実装ともwall timeとstream latencyがconsumer sleepに支配される。
- native stream resourceはfirst yieldでcurl/ext-grpcと同等以上の位置にあり、slow consumer条件で先に全responseをdrainする挙動は見えない。
- native stream resourceのRSS max deltaは同条件でcurlより小さく、ext-grpcより大幅に小さい。ただし `ru_maxrss` はprocess high-waterなので、長時間・多streamの厳密なmemory upper bound証明には別途stressが必要。
- release gateとしては「代表slow consumer条件でconsumer-speed limitedになり、memoryが明確に増え続けないこと」を確認済み。長時間stressはrelease hardeningとして残す。
