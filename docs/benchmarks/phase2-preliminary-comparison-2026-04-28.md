# Phase 2 preliminary comparison 2026-04-28

Phase 2 探索ベンチを `php-grpc-lite` と公式 `ext-grpc` で一通り実行した preliminary 記録。

この結果はスコープ判断の初期材料であり、regression baseline ではない。数値そのものを目標値にせず、どの軸に改善余地があるかを見る。

## 条件

| 項目 | 内容 |
|---|---|
| 実行日 | 2026-04-28 |
| 実行入口 | `bench/phase2/compare.sh` |
| 保存 tag | `phase2-compare-20260428` |
| 対向 | Go test-server |
| 比較対象 | `php-grpc-lite` / 公式 `ext-grpc` |
| 保存先 | `var/bench-results/phase2-*-phase2-compare-20260428-*.json` |

JSON は `docs/benchmarks/schemas/phase2-result-v1.md` の contract で検証済み。

## 実行コマンド

```bash
BENCH_TAG=phase2-compare-20260428 ./bench/phase2/compare.sh throughput-unary --duration=1 --warmup-calls=3
BENCH_TAG=phase2-compare-20260428 ./bench/phase2/compare.sh payload-unary --duration=0.5 --payload-sizes=0,100,1024,10240,102400 --warmup-calls=2
BENCH_TAG=phase2-compare-20260428 ./bench/phase2/compare.sh rtt-unary --calls=10 --warmup-calls=2
BENCH_TAG=phase2-compare-20260428 ./bench/phase2/compare.sh throughput-streaming --duration=0.5 --message-count=100 --payload-bytes=100 --warmup-streams=1
BENCH_TAG=phase2-compare-20260428 ./bench/phase2/compare.sh large-streaming --message-counts=1000,10000 --payload-bytes=100
BENCH_TAG=phase2-compare-20260428 ./bench/phase2/compare.sh payload-streaming --streams=5 --message-count=100 --payload-sizes=0,100,1024,10240
BENCH_TAG=phase2-compare-20260428 ./bench/phase2/compare.sh metadata-header --calls=10
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
| large unary payload | 100KB p99 が怪しい | per-byte decode / copy と tail を再測 |
| RTT / cold | direct cold は ext-grpc 優位 | persistent pool / request 境界の影響を decision run で確認 |
| streaming throughput | count が大きいほど php-grpc-lite 優位に見える | 10K / 100K messages で長めに再測 |
| streaming tail | php-grpc-lite p99 が悪いケースあり | stream latency distribution を確認 |
| metadata volume | header 増加で固定費が増える | calls=100 以上で p50/p95/p99 を安定化 |

## 次の打ち手

1. `bench/phase2/preset.sh compare` で今回相当の短時間比較を再現可能にする。
2. `bench/phase2/preset.sh decision` で改善判断用の長めの比較を取る。
3. decision 結果で `curl handle reuse` の効果を見る軸を決め、実装後に同じ preset で差分を見る。
