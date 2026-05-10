# Benchmark preliminary comparison 2026-04-28

Phase 2 探索ベンチを `php-grpc-lite` と公式 `ext-grpc` で一通り実行した preliminary 記録。

この結果はスコープ判断の初期材料であり、regression baseline ではない。数値そのものを目標値にせず、どの軸に改善余地があるかを見る。

## 条件

| 項目 | 内容 |
|---|---|
| 実行日 | 2026-04-28 |
| 実行入口 | `bench/compare.sh` |
| 保存 tag | `phase2-compare-20260428` |
| 対向 | Go test-server |
| 比較対象 | `php-grpc-lite` / 公式 `ext-grpc` |
| 保存先 | `var/bench-results/phase2-*-phase2-compare-20260428-*.json` |

JSON は `docs/benchmarks/schemas/phase2-result-v1.md` の contract で検証済み。

## 実行コマンド

```bash
BENCH_TAG=phase2-compare-20260428 ./bench/compare.sh throughput-unary --duration=1 --warmup-calls=3
BENCH_TAG=phase2-compare-20260428 ./bench/compare.sh payload-unary --duration=0.5 --payload-sizes=0,100,1024,10240,102400 --warmup-calls=2
BENCH_TAG=phase2-compare-20260428 ./bench/compare.sh rtt-unary --calls=10 --warmup-calls=2
BENCH_TAG=phase2-compare-20260428 ./bench/compare.sh throughput-streaming --duration=0.5 --message-count=100 --payload-bytes=100 --warmup-streams=1
BENCH_TAG=phase2-compare-20260428 ./bench/compare.sh large-streaming --message-counts=1000,10000 --payload-bytes=100
BENCH_TAG=phase2-compare-20260428 ./bench/compare.sh payload-streaming --streams=5 --message-count=100 --payload-sizes=0,100,1024,10240
BENCH_TAG=phase2-compare-20260428 ./bench/compare.sh metadata-header --calls=10
```

## Unary

### Throughput

| implementation | calls | calls/sec | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| php-grpc-lite | 27049 | 27048.5 | 32.7μs | 50.6μs | 70.5μs |
| ext-grpc | 17379 | 17378.7 | 53.3μs | 81.5μs | 96.1μs |

軽量 unary では php-grpc-lite の方が高 throughput / 低 latency。ここだけを見ると C 化の最優先候補を per-call fixed cost と断定する根拠は弱い。

### Payload

| payload | php-grpc-lite calls/sec | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc calls/sec | ext-grpc p50 | ext-grpc p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 0B | 27742.5 | 32.7μs | 69.7μs | 17838.9 | 51.6μs | 99.0μs |
| 100B | 27356.6 | 32.4μs | 65.7μs | 17237.2 | 56.5μs | 94.0μs |
| 1024B | 27355.7 | 32.5μs | 68.3μs | 16133.7 | 59.2μs | 92.9μs |
| 10240B | 20953.4 | 35.7μs | 253.9μs | 14122.8 | 62.8μs | 285.9μs |
| 102400B | 5749.6 | 81.2μs | 2429.4μs | 5941.1 | 103.5μs | 1467.0μs |

100KB では throughput がほぼ同等で、php-grpc-lite は p50 が低い一方 p99 が悪い。per-byte decode / copy と tail の分解が次の確認対象。

### Payload breakdown

`payload-breakdown` は RPC 全体ではなく、ネットワークを外した PHP 内 hot path の diagnostic。`payload-unary` の tail が悪い場合に、frame length read、payload slice、protobuf decode、deserialize adapter のどこが支配的かを見る。

```bash
BENCH_TAG=phase2-payload-breakdown-20260428 ./bench/run.sh payload-breakdown --payload-sizes=0,100,1024,10240,102400
```

| payload | operation | p50 | p95 | p99 |
|---:|---|---:|---:|---:|
| 0B | frame length | 0.0μs | 0.1μs | 0.2μs |
| 0B | payload slice | 0.1μs | 0.1μs | 0.1μs |
| 0B | decode only | 0.2μs | 0.3μs | 0.3μs |
| 0B | slice + decode | 0.3μs | 0.4μs | 0.4μs |
| 100KB | frame length | 0.0μs | 0.1μs | 0.1μs |
| 100KB | payload slice | 1.6μs | 1.6μs | 2.0μs |
| 100KB | decode only | 5.2μs | 5.4μs | 7.4μs |
| 100KB | slice + decode | 5.6μs | 7.8μs | 8.1μs |
| 100KB | deserialize apply | 5.2μs | 5.4μs | 7.1μs |

この diagnostic では 100KB でも frame/slice/decode は p99 で 10μs 未満。`payload-unary` の 100KB p99 が ms 単位で悪い件は、protobuf decode 単体よりも RPC I/O、curl buffering、allocation / GC、または Docker scheduler の tail と見た方が妥当。

### Payload unary RPC diagnostic

`payload-unary-diagnostic` は実 RPC の php-grpc-lite unary 経路に opt-in instrumentation を入れ、`curl_exec`、body append、frame parse、payload slice、deserialize の時間を call ごとに保存する。通常の比較値を壊さないよう、公式 ext-grpc との比較 suite ではなく php-grpc-lite 単独 diagnostic として実行する。

```bash
BENCH_TAG=phase2-payload-rpc-diagnostic-20260429 ./bench/run.sh payload-unary-diagnostic --duration=0.2 --payload-sizes=102400 --warmup-calls=1
```

| metric | p99 |
|---|---:|
| total unary latency | 2362.4μs |
| `curl_exec` | 2350.9μs |
| body chunks | 13 |
| body append total | 8.2μs |
| body append max | 4.2μs |
| body bytes total | 102409B |
| largest body chunk | 16375B |
| response frame length read | 0.5μs |
| response payload slice | 5.9μs |
| response deserialize | 6.3μs |
| status build | 1.1μs |

この run では、100KB の p99 tail はほぼ `curl_exec` 内にある。body append、frame parse、payload slice、protobuf deserialize はいずれも μs 台で、ms 単位の tail を説明しない。次の確認対象は libcurl / HTTP/2 receive、connection reuse 状態、Docker scheduler、Go test-server 側の送信 pacing / buffering。

### RTT

| scenario | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc p50 | ext-grpc p99 |
|---|---:|---:|---:|---:|
| warm direct | 51.4μs | 100.0μs | 95.5μs | 1199.1μs |
| cold direct | 240.8μs | 302.9μs | 68.0μs | 108.9μs |
| warm 1ms | 1965.5μs | 2027.8μs | 1945.9μs | 2145.2μs |
| cold 1ms | 1967.5μs | 2461.1μs | 1480.9μs | 2201.7μs |
| warm 3ms | 4408.1μs | 4719.5μs | 4165.6μs | 4981.7μs |
| cold 3ms | 4802.4μs | 5170.1μs | 4416.3μs | 5036.0μs |
| warm 5ms | 6012.5μs | 7017.6μs | 6494.5μs | 7040.2μs |
| cold 5ms | 6987.4μs | 7600.6μs | 6010.2μs | 7014.5μs |

Toxiproxy による downstream latency 近似なので、実ネットワーク RTT の完全再現ではない。latency を入れると両者とも遅延支配になり、direct cold では ext-grpc が軽い。

## Server streaming

### Throughput

| implementation | streams | messages/sec | p50 stream | p99 stream |
|---|---:|---:|---:|---:|
| php-grpc-lite | 1423 | 284453.4 | 298.2μs | 1585.6μs |
| ext-grpc | 1444 | 288622.2 | 330.1μs | 769.9μs |

100 messages / stream では throughput はほぼ同等。php-grpc-lite は p50 が低いが p99 が悪い。

### Large streaming

| message count | php-grpc-lite elapsed | php-grpc-lite msg/sec | ext-grpc elapsed | ext-grpc msg/sec |
|---:|---:|---:|---:|---:|
| 1000 | 3.0ms | 333236.0 | 7.1ms | 141136.1 |
| 10000 | 8.1ms | 1239936.1 | 24.4ms | 409482.4 |

この run では large streaming は php-grpc-lite が明確に速い。計測が短すぎる可能性があるため、decision preset では 10K / 100K messages で再測する。

### Payload streaming

| payload | php-grpc-lite msg/sec | php-grpc-lite p50 stream | ext-grpc msg/sec | ext-grpc p50 stream |
|---:|---:|---:|---:|---:|
| 0B | 185722.7 | 382.2μs | 149772.3 | 370.0μs |
| 100B | 242997.2 | 382.3μs | 271005.4 | 371.7μs |
| 1024B | 146231.8 | 412.1μs | 222456.9 | 440.7μs |
| 10240B | 90165.4 | 780.5μs | 88770.8 | 1057.4μs |

payload streaming は mixed。1KB では ext-grpc の msg/sec が高く、10KB では throughput は同等で php-grpc-lite の p50 が低い。

## Metadata

| scenario | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc p50 | ext-grpc p99 |
|---|---:|---:|---:|---:|
| req 0 / resp 0 / value 0B | 62.4μs | 4241.3μs | 81.9μs | 1898.1μs |
| req 10 / resp 0 / value 32B | 55.3μs | 108.2μs | 67.6μs | 97.3μs |
| req 50 / resp 0 / value 32B | 118.4μs | 194.4μs | 101.6μs | 2175.1μs |
| req 10 / resp 10 / value 32B | 80.8μs | 133.7μs | 84.5μs | 139.7μs |
| req 50 / resp 50 / value 32B | 244.5μs | 311.3μs | 212.8μs | 239.1μs |

header 数の増加に対して両者とも p50 が上がる。calls=10 では p99 の外れ値影響が大きいため、判断用には calls=100 以上で再測する。

## 暫定判断

| 観測軸 | 暫定判断 | 次の確認 |
|---|---|---|
| 軽量 unary fixed cost | php-grpc-lite は既に良好 | curl handle reuse は cold/warm 差の変化を見る目的で実施 |
| large unary payload | 100KB p99 が怪しい | 実 RPC diagnostic では `curl_exec` 内 tail が支配的。libcurl / HTTP/2 receive と scheduler を確認 |
| RTT / cold | direct cold は ext-grpc 優位 | persistent pool / request 境界の影響を decision run で確認 |
| streaming throughput | count が大きいほど php-grpc-lite 優位に見える | 10K / 100K messages で長めに再測 |
| streaming tail | php-grpc-lite p99 が悪いケースあり | stream latency distribution を確認 |
| metadata volume | header 増加で固定費が増える | calls=100 以上で p50/p95/p99 を安定化 |

## 次の打ち手

1. `bench/preset.sh compare` で今回相当の短時間比較を再現可能にする。
2. `bench/preset.sh decision` で改善判断用の長めの比較を取る。
3. decision 結果で `curl handle reuse` の効果を見る軸を決め、実装後に同じ preset で差分を見る。

`payload-breakdown` と `payload-unary-diagnostic` の初回結果では、decode 単体と body accumulation は支配的ではなかった。次に見るべきは、100KB の実 RPC で libcurl / HTTP/2 receive と接続状態が tail を作っているかどうか。
