# nghttp2 PoC: receive buffer size safety check (2026-05-03)

## 目的

`recv-buffer-size=256KiB` が call type / payload size に関わらず常に改善するかを確認する。

固定条件:

- nghttp2 PoC
- poll loop
- receive stream / connection window: 16MiB
- `flush-after-mem-recv`
- no-copy request path
- Go test-server default settings

比較:

- default receive buffer: 16KiB
- larger receive buffer: 256KiB

## 1回目の sweep

| case | 16KiB p50 | 16KiB p99 | 16KiB throughput | 256KiB p50 | 256KiB p99 | 256KiB throughput | 判断 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| unary 100B | 34.0μs | 409.0μs | 18857.4 calls/s | 30.0μs | 272.0μs | 23746.0 calls/s | 改善 |
| unary 100KiB | 67.0μs | 1463.0μs | 7844.8 calls/s | 65.0μs | 1312.0μs | 8258.3 calls/s | 改善 |
| unary 1MiB | 349.0μs | 3401.0μs | 1795.1 calls/s | 330.0μs | 3344.0μs | 1854.2 calls/s | 改善 |
| stream 1000×100B | 4437.0μs | 9639.0μs | 216.2 calls/s | 4169.0μs | 6983.0μs | 231.1 calls/s | 改善 |
| stream 10×100KiB | 466.0μs | 3945.0μs | 1206.5 calls/s | 444.0μs | 3735.0μs | 1266.9 calls/s | 改善 |
| stream 1×1MiB | 431.0μs | 4171.0μs | 1387.6 calls/s | 453.0μs | 3908.0μs | 1314.0 calls/s | p99改善、p50/throughput悪化 |

1回目の sweep では大半のケースで改善したが、stream 1×1MiB だけ p50 と throughput が悪化した。

## stream 1×1MiB 反復

stream 1×1MiB は悪化の疑いがあるため、3回反復した。

| receive buffer | rep | p50 | p99 | throughput |
| --- | ---: | ---: | ---: | ---: |
| 16KiB | 1 | 483.0μs | 4023.0μs | 1303.4 calls/s |
| 16KiB | 2 | 447.0μs | 3804.0μs | 1430.5 calls/s |
| 16KiB | 3 | 424.0μs | 3924.0μs | 1434.5 calls/s |
| 256KiB | 1 | 434.0μs | 3968.0μs | 1409.9 calls/s |
| 256KiB | 2 | 421.0μs | 4336.0μs | 1338.0 calls/s |
| 256KiB | 3 | 420.0μs | 3815.0μs | 1421.5 calls/s |

平均:

| receive buffer | p50 avg | p99 avg | throughput avg |
| --- | ---: | ---: | ---: |
| 16KiB | 451.3μs | 3917.0μs | 1389.5 calls/s |
| 256KiB | 425.0μs | 4039.7μs | 1389.8 calls/s |

## 判断

`recv-buffer-size=256KiB` は常に性能向上するとは言えない。

- small unary / 100KiB unary / 1MiB unary / many small streaming / 10×100KiB streaming では改善した。
- stream 1×1MiB では p50 は改善したが、反復平均の p99 は 16KiB より悪化した。
- throughput 平均はほぼ同等だった。

したがって native transport の default に 256KiB を固定採用するにはまだ弱い。

実装判断としては以下が妥当。

1. receive buffer size は固定最適値ではなく、transport option / tuning knob として残す。
2. 初期 default は 16KiB または保守的な値にし、large streaming 専用の tuning として 256KiB を扱う。
3. regression baseline に small unary / large unary / many-small streaming / large streaming を入れて、default 変更時だけ受け入れ判断する。

receive window 拡大と `flush-after-mem-recv` は今回の確認でも主効果として残るが、receive buffer 256KiB は call shape 依存である。
