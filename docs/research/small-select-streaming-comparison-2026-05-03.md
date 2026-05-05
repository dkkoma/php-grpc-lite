# Small SELECT Streaming Comparison 2026-05-03

## Purpose

Spanner `ExecuteStreamingSql` の小さい `SELECT` 結果を近似する server streaming 形状で、以下の系統を同じ Go test-server 条件で比較した。

- `php-grpc-lite` curl transport
- `php-grpc-lite` HTTP/2 transport
- official `ext-grpc`
- HTTP/2 transport PoC compact/direct variants

これは Spanner emulator 実測ではなく、`BenchServerStream` による制御可能な近似である。small SELECT軸では1つの `PartialResultSet` 相当で返る形だけを見る。複数messageへの分割やchunkingは、many-small / streaming count系の別ベンチで扱う。

small streamingは実運用上の主経路であり、large payloadだけでHTTP/2 transportを採用判断しない。small SELECT相当のactual surfaceでext-grpc同等または優位を示せない状態はrelease defaultの条件を満たさない。

Spanner emulator上の実測では、2 DATE + 2 STRING + 合計10 columnsの1行SELECTは1つの `PartialResultSet` / 267Bだった。詳細は `docs/research/spanner-emulator-streaming-shape-2026-05-03.md`。

## Command

```bash
BENCH_TAG=20260503-small-select-streaming-poc bench/phase2/compare-small-select-streaming.sh
```

HTTP/2 transport は `NATIVE_RESPONSE_MODE=compact64` のdefaultで計測した。

Summary TSV:

```text
var/bench-results/phase2-small-select-streaming-20260503-small-select-streaming-poc.tsv
```

## Results

| case | implementation | variant | p50 us | p99 us | msg/s | server last p99 us | client residual p99 us |
|---|---|---:|---:|---:|---:|---:|---:|
| 1x100b | php-grpc-lite | curl | 206.1 | 1711.6 | 3559.2 | 27.7 | 1683.9 |
| 1x100b | php-grpc-lite | native-compact64 | 139.2 | 1591.4 | 4874.5 | 22.4 | 1568.9 |
| 1x100b | ext-grpc | c-core | 74.4 | 893.0 | 9891.6 | 19.8 | 873.2 |
| 1x100b | php-grpc-lite | poc-compact64 | 31.0 | 369.0 | 23397.3 | 20.5 | 348.5 |
| 1x100b | php-grpc-lite | poc-direct | 30.0 | 328.0 | 19659.1 | 20.8 | 307.2 |
| 1x1k | php-grpc-lite | curl | 209.9 | 1557.6 | 3605.0 | 29.1 | 1528.5 |
| 1x1k | php-grpc-lite | native-compact64 | 143.0 | 1519.5 | 4594.0 | 27.6 | 1491.9 |
| 1x1k | ext-grpc | c-core | 81.9 | 673.5 | 9519.2 | 21.5 | 652.0 |
| 1x1k | php-grpc-lite | poc-compact64 | 33.0 | 576.0 | 19836.9 | 35.9 | 540.1 |
| 1x1k | php-grpc-lite | poc-direct | 34.0 | 286.0 | 22253.9 | 32.0 | 254.0 |
| 1x4k | php-grpc-lite | curl | 205.8 | 1710.4 | 3418.5 | 48.3 | 1662.1 |
| 1x4k | php-grpc-lite | native-compact64 | 140.8 | 1649.1 | 4325.1 | 48.5 | 1600.6 |
| 1x4k | ext-grpc | c-core | 79.1 | 650.3 | 10110.7 | 43.9 | 606.5 |
| 1x4k | php-grpc-lite | poc-compact64 | 33.0 | 491.0 | 20077.9 | 24.8 | 466.2 |
| 1x4k | php-grpc-lite | poc-direct | 34.0 | 780.0 | 18565.3 | 24.6 | 755.4 |
| 1x10k | php-grpc-lite | curl | 206.1 | 1765.1 | 3323.9 | 277.6 | 1487.6 |
| 1x10k | php-grpc-lite | native-compact64 | 139.7 | 2033.4 | 4101.7 | 73.9 | 1959.4 |
| 1x10k | ext-grpc | c-core | 85.0 | 993.4 | 8207.7 | 77.1 | 916.3 |
| 1x10k | php-grpc-lite | poc-compact64 | 35.0 | 572.0 | 18819.3 | 204.4 | 367.6 |
| 1x10k | php-grpc-lite | poc-direct | 36.0 | 693.0 | 15203.6 | 182.8 | 510.2 |

## Interpretation

- actual HTTP/2 surfaceはp50ではcurlより速いが、ext-grpcには届いていない。small SELECT相当ではこの状態をrelease default可能とは見なさない。
- PoC direct/compactは `1x100b` / `1x1k` / `1x4k` でext-grpc同等以上のp50/p99とthroughputを示した。HTTP/2 transport自体にsmallで勝てない構造的制約がある、とはまだ言えない。
- `1x10k` はPoC directがext-grpcとほぼ同等レンジ、compactはtailが悪い。payloadが少し大きい単一messageではdirect寄りが有利。
- 主なギャップはPoC transport pathとactual `ServerStreamingCall::responses()` surfaceの間にある。true streaming resource化とmessage delivery strategyが次の主対象になる。

## Follow-up

- small SELECT実用形状では `1x1k` / `1x4k` を代表ケースにする。
- native actual surfaceでtrue streaming resourceを実装した後、このrunnerで再測定する。
- message分割ケースはsmall SELECT軸には混ぜず、many-small / streaming count系の別ベンチで確認する。
