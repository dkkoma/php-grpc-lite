# Small SELECT Streaming Comparison 2026-05-03

## Purpose

Spanner `ExecuteStreamingSql` の小さい `SELECT` 結果を近似する server streaming 形状で、以下の3系統を同じ Go test-server 条件で比較した。

- `php-grpc-lite` curl transport
- `php-grpc-lite` native transport
- official `ext-grpc`

これは Spanner emulator 実測ではなく、`BenchServerStream` による制御可能な近似である。`1x1k` / `1x4k` は小さい結果が1つの `PartialResultSet` 相当で返る形、`10x1k` / `10x4k` は小さい結果が複数messageへ分割される形を見る。

## Command

```bash
BENCH_TAG=20260503-small-select-streaming bench/phase2/compare-small-select-streaming.sh
```

Native transport は `NATIVE_RESPONSE_MODE=compact64` のdefaultで計測した。

Summary TSV:

```text
var/bench-results/phase2-small-select-streaming-20260503-small-select-streaming.tsv
```

## Results

| case | implementation | variant | p50 us | p99 us | msg/s | server last p99 us | client residual p99 us |
|---|---|---:|---:|---:|---:|---:|---:|
| 1x100b | php-grpc-lite | curl | 217.8 | 1995.9 | 3127.8 | 68.8 | 1927.0 |
| 1x100b | php-grpc-lite | native-compact64 | 142.0 | 1523.9 | 4657.7 | 36.2 | 1487.7 |
| 1x100b | ext-grpc | c-core | 72.6 | 589.1 | 11006.7 | 13.8 | 575.3 |
| 1x1k | php-grpc-lite | curl | 202.7 | 1516.9 | 3637.7 | 28.0 | 1488.9 |
| 1x1k | php-grpc-lite | native-compact64 | 134.2 | 1669.3 | 4652.8 | 32.8 | 1636.6 |
| 1x1k | ext-grpc | c-core | 74.9 | 701.2 | 10178.6 | 22.0 | 679.3 |
| 1x4k | php-grpc-lite | curl | 203.2 | 1652.3 | 3560.2 | 171.5 | 1480.8 |
| 1x4k | php-grpc-lite | native-compact64 | 142.9 | 1522.0 | 4474.1 | 206.5 | 1315.5 |
| 1x4k | ext-grpc | c-core | 81.6 | 823.4 | 9238.8 | 45.5 | 777.8 |
| 1x10k | php-grpc-lite | curl | 210.6 | 1876.9 | 3262.8 | 252.2 | 1624.8 |
| 1x10k | php-grpc-lite | native-compact64 | 144.8 | 2101.5 | 3850.3 | 63.2 | 2038.3 |
| 1x10k | ext-grpc | c-core | 94.1 | 1214.8 | 6801.7 | 253.0 | 961.9 |
| 10x1k | php-grpc-lite | curl | 243.9 | 1593.4 | 28071.8 | 857.1 | 736.3 |
| 10x1k | php-grpc-lite | native-compact64 | 183.6 | 1776.3 | 34182.4 | 348.6 | 1427.8 |
| 10x1k | ext-grpc | c-core | 142.5 | 1842.3 | 44156.9 | 1302.0 | 540.4 |
| 10x4k | php-grpc-lite | curl | 275.5 | 2137.0 | 24218.4 | 720.8 | 1416.2 |
| 10x4k | php-grpc-lite | native-compact64 | 198.4 | 1876.8 | 30935.9 | 1258.2 | 618.6 |
| 10x4k | ext-grpc | c-core | 151.8 | 1510.2 | 46313.1 | 965.7 | 544.5 |

## Interpretation

- p50ではnativeがcurlより一貫して速い。small streamingでもlibcurlを外す効果は見える。
- ext-grpcは単一message系のp50/p99でまだ明確に速い。small SELECT相当ではtransport固定費とPHP surface側のmessage delivery差が残っている。
- `10x1k` のp99はext-grpcのserver last p99が高く、total p99だけでclient差を読むと誤る。client residual p99ではext-grpcが最も低い。
- `10x4k` はnativeのclient residual p99がext-grpcにかなり近い。複数messageである程度payloadがある形ではnative compact pathの効果が出ている。
- この系統はlarge response最適化の確認ではなく、Spanner-like small streaming shapeの固定費確認として扱う。

## Follow-up

- small SELECT実用形状では `1x1k` / `1x4k` を代表ケースにする。
- chunked small resultの確認には `10x1k` / `10x4k` を使う。
- native actual surfaceでtrue streaming resourceを実装した後、このrunnerで再測定する。
