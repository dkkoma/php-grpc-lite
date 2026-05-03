# native surface / 100x100KiB repeat check (2026-05-03)

## 目的

レビュー指摘のうち判断不要で進められるものとして、以下を追加した。

- PoC batch APIだけではなく、actual `UnaryCall::wait()` / `ServerStreamingCall::responses()` surfaceを通るnative測定variant。
- `100×100KiB` server streaming例外ケースのfocused repeat runner。
- native surfaceでもserver stats trailerを回収できるよう、`NativeTransport` wrapperから `x-bench-*` trailer相当を公開。
- transport選択の制御テスト。

## 追加した入口

三者比較runnerは `native-surface` variantを出す。

```bash
BENCH_TAG=20260503-native-mvp-vs-libcurl-ext bench/phase2/compare-native-mvp-vs-libcurl-ext.sh
```

`100×100KiB` の例外形状だけを同じserver条件で繰り返す。

```bash
REPEATS=3 BENCH_TAG=20260503-100x100k-repeat bench/phase2/repeat-server-stream-100x100k.sh
```

## Smoke

actual native surfaceの最小疎通:

| case | p50 | p99 | throughput |
| --- | ---: | ---: | ---: |
| unary 100B, 3 calls | 173.2μs | 242.4μs | 5284.7 calls/s |
| stream 2×3×100B | 199.6μs | 1014.8μs | 4914.4 msg/s |

これは性能判断用ではなく、extension load + `wait()` / `responses()` surfaceの疎通確認である。

## 100×100KiB repeat result

Command:

```bash
REPEATS=1 BENCH_TAG=20260503-native-surface-repeat-smoke bench/phase2/repeat-server-stream-100x100k.sh
```

| implementation | mode | p50 | p99 | msg/s | server last p99 |
| --- | --- | ---: | ---: | ---: | ---: |
| php-grpc-lite | curl | 5599.8μs | 12658.5μs | 16156.5 | 10606.4μs |
| php-grpc-lite | native-direct | 6425.8μs | 14252.9μs | 14640.9 | 13324.4μs |
| php-grpc-lite | native-compact64 | 6742.3μs | 14314.0μs | 14220.7 | 13472.7μs |
| ext-grpc | c-core | 5955.2μs | 11961.0μs | 15857.8 | 11127.0μs |

出力:

- `var/bench-results/phase2-server-stream-100x100k-repeat-20260503-native-surface-repeat-smoke.tsv`

## 見解

`100×100KiB` は今回のactual surface repeatでもnativeが勝っていない。native-direct / native-compact64のclient p99悪化と同時に `server last p99` も悪化しているため、単純なPHP decode/yieldだけの差ではない。

この形状はnative default判断のブロッカーというより、server/transport progression差が混ざりやすい例外形状としてdecision runでrepeat数を増やして扱うべきである。

## 残り

- slow consumer / backpressureのactual native surface検証。
- deadline / cancel / RST_STREAM / missing trailers / TLS / mTLS のnative実装と互換テスト。
- direct / compact のdefault選択ルール。
