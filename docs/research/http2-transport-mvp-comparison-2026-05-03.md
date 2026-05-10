# HTTP/2 transport MVP comparison (2026-05-03)

## 目的

Phase 2 のチェックポイントとして、以下の三者を同じGo test-server条件で比較した。

1. current `php-grpc-lite` libcurl transport。
2. nghttp2 direct MVP extension PoC。
3. official `ext-grpc`。

ここでのMVPはproduction API完成版ではなく、`poc/nghttp2-client-ext` のHTTP/2経路を使った比較対象である。request uploadは `no-copy + poll loop`、server streaming responseは `direct payload assembly` と `compact64` を代表パターンとして測った。

## 再実行方法

```bash
BENCH_TAG=20260503-native-mvp-vs-libcurl-ext bench/compare-native-mvp-vs-libcurl-ext.sh
```

出力:

- summary: `var/bench-results/phase2-native-mvp-vs-libcurl-ext-20260503-native-mvp-vs-libcurl-ext.tsv`
- per-run JSON: `var/bench-results/phase2-native-mvp-vs-libcurl-ext-20260503-native-mvp-vs-libcurl-ext-*.json`

## large request unary

条件:

- `BenchUnary`
- request payload: `1MiB`
- response payload: `100B`
- warm sequential calls

| metric | libcurl | MVP upload | ext-grpc |
| --- | ---: | ---: | ---: |
| p50 | 844.2μs | 360.0μs | 412.9μs |
| p99 | 4065.3μs | 3839.0μs | 3642.6μs |
| throughput | 925.7 calls/s | 1396.0 calls/s | 1370.4 calls/s |

判断:

- MVP uploadはp50/calls/sでext-grpcを上回る。
- p99はext-grpcより約196μs遅いが、libcurlよりは改善している。
- large request pathでlibcurlを外す価値は引き続き明確。

## server streaming summary

| case | metric | libcurl | MVP direct | MVP compact64 | ext-grpc |
| --- | --- | ---: | ---: | ---: | ---: |
| 1000×100B | p50 | 4604.2μs | 4368.0μs | 4431.0μs | 5095.2μs |
| 1000×100B | p99 | 8385.6μs | 8131.0μs | 8347.0μs | 9038.2μs |
| 1000×100B | msg/s | 207866.0 | 219781.8 | 213674.5 | 188950.6 |
| 1000×100B | server last p99 | 7955.0μs | 7858.5μs | 8308.6μs | 7768.3μs |
| 10×100KiB | p50 | 917.4μs | 480.0μs | 506.0μs | 633.9μs |
| 10×100KiB | p99 | 3639.9μs | 3465.0μs | 3340.0μs | 3379.3μs |
| 10×100KiB | msg/s | 8318.2 | 12219.2 | 12620.3 | 10965.5 |
| 10×100KiB | server last p99 | 3016.1μs | 3277.1μs | 3227.2μs | 2874.9μs |
| 100×100KiB | p50 | 5062.3μs | 5473.0μs | 5393.0μs | 5407.4μs |
| 100×100KiB | p99 | 10341.3μs | 12747.0μs | 12396.0μs | 10760.4μs |
| 100×100KiB | msg/s | 18210.2 | 16702.3 | 16773.6 | 17523.1 |
| 100×100KiB | server last p99 | 9837.8μs | 12611.1μs | 12264.4μs | 10631.9μs |
| 1×1MiB | p50 | 899.0μs | 486.0μs | 491.0μs | 527.7μs |
| 1×1MiB | p99 | 5406.4μs | 3985.0μs | 4109.0μs | 3852.0μs |
| 1×1MiB | msg/s | 792.3 | 1281.0 | 1316.6 | 1277.2 |
| 1×1MiB | server last p99 | 4484.3μs | 3675.4μs | 3582.6μs | 3250.2μs |
| 10000×100B | p50 | 43996.8μs | 42052.0μs | 41925.0μs | 43531.5μs |
| 10000×100B | p99 | 56445.6μs | 50780.0μs | 53243.0μs | 57483.6μs |
| 10000×100B | msg/s | 224598.7 | 233831.1 | 235003.4 | 226376.7 |
| 10000×100B | server last p99 | 55852.4μs | 50659.6μs | 52900.7μs | 56125.7μs |

## server streaming MVP internals

| case | metric | MVP direct | MVP compact64 |
| --- | --- | ---: | ---: |
| 1000×100B | poll wait p99 | 7584μs | 7742μs |
| 1000×100B | max body buffer p99 | 0B | 65591B |
| 10×100KiB | poll wait p99 | 3299μs | 3205μs |
| 10×100KiB | max body buffer p99 | 0B | 102409B |
| 100×100KiB | poll wait p99 | 10865μs | 10777μs |
| 100×100KiB | max body buffer p99 | 0B | 102409B |
| 1×1MiB | poll wait p99 | 3835μs | 3801μs |
| 1×1MiB | max body buffer p99 | 0B | 1048585B |
| 10000×100B | poll wait p99 | 45354μs | 47303μs |
| 10000×100B | max body buffer p99 | 0B | 65591B |

## server streaming winners

| case | best p50 | best p99 | best throughput | note |
| --- | --- | --- | --- | --- |
| 1000×100B | MVP direct | MVP direct | MVP direct | MVPが全指標で優位 |
| 10×100KiB | MVP direct | MVP compact64 | MVP compact64 | MVPが全指標で優位 |
| 100×100KiB | libcurl | libcurl | libcurl | 今回runではMVPが悪い。server last p99も同時に悪化 |
| 1×1MiB | MVP direct | ext-grpc | MVP compact64 | p99だけext-grpcが僅差で優位 |
| 10000×100B | MVP compact64 | MVP direct | MVP compact64 | MVPが全指標で優位 |

## server streaming 判断

MVPは多くのstreaming shapeでext-grpcと同等以上にいる。

- `1000×100B`: MVP direct/compact64ともext-grpcよりp50/p99/msg/sが良い。
- `10×100KiB`: MVP compact64はext-grpcよりp99がわずかに良く、msg/sも高い。
- `1×1MiB`: MVP directはext-grpcとほぼ同等。p50とmsg/sはMVPが良く、p99はext-grpcが約133μs良い。
- `10000×100B`: MVP direct/compact64ともext-grpcより良い。
- `100×100KiB`: このrunではlibcurlが最良、ext-grpcが次点、MVPは悪い。server last p99も同時に悪化しているため、client内CPUだけの問題ではなくreceive progression / server send progressの揺れを含む。

`100×100KiB` は前回 `poc-goal` 比較ではMVP compact64がext-grpc同等だったため、run間揺れが大きい。最終判断前にrepeatまたはdecision runで再取得する価値がある。

## 総合判断

HTTP/2 transport MVPはPhase 2の実装方向として十分成立する。

- large request unaryではlibcurlを明確に上回り、ext-grpcと同等レンジ。
- server streaming large responseでは多くの代表形状でext-grpc同等以上。
- response pathは `direct payload assembly` と `compact/ring buffer` の両方が必要。
- default transportはHTTP/2へ進める判断でよい。
- libcurl経路は自動fallbackではなく、workload選択・安定経路・互換性oracleとして `php_grpc_lite.transport=curl` で明示的に残す。

ただしMVPのproduction化には以下が残る。

- `nghttp2_poc_unary_batch()` は計測用APIであり、production API/lifetimeへ作り直す必要がある。
- current opt-in PHP wrapperはsmoke用で、connection/session lifetimeをまだChannelに持たない。
- TLS/mTLS/deadline/metadata/status validation/error semanticsはlibcurl実装同等に移植が必要。
- server streamingのactual `responses()` incremental delivery surfaceで再計測が必要。
