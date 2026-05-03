# native transport MVP comparison (2026-05-03)

## 目的

Phase 2 のチェックポイントとして、以下の三者を同じGo test-server条件で比較した。

1. current `php-grpc-lite` libcurl transport。
2. nghttp2 native MVP extension PoC。
3. official `ext-grpc`。

ここでのMVPはproduction API完成版ではなく、`poc/nghttp2-client-ext` のnative nghttp2経路を使った比較対象である。request uploadは `no-copy + poll loop`、server streaming responseは `direct payload assembly` と `compact64` を代表パターンとして測った。

## 再実行方法

```bash
BENCH_TAG=20260503-native-mvp-vs-libcurl-ext bench/phase2/compare-native-mvp-vs-libcurl-ext.sh
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

| implementation | p50 | p99 | calls/s |
| --- | ---: | ---: | ---: |
| libcurl | 844.2μs | 4065.3μs | 925.7 |
| MVP upload | 360.0μs | 3839.0μs | 1396.0 |
| ext-grpc | 412.9μs | 3642.6μs | 1370.4 |

判断:

- MVP uploadはp50/calls/sでext-grpcを上回る。
- p99はext-grpcより約196μs遅いが、libcurlよりは改善している。
- large request pathでlibcurlを外す価値は引き続き明確。

## server streaming

| case | implementation | p50 | p99 | msg/s | server last p99 | poll wait p99 | max body buffer p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1000×100B | libcurl | 4604.2μs | 8385.6μs | 207866.0 | 7955.0μs | - | - |
| 1000×100B | MVP direct | 4368.0μs | 8131.0μs | 219781.8 | 7858.5μs | 7584μs | 0B |
| 1000×100B | MVP compact64 | 4431.0μs | 8347.0μs | 213674.5 | 8308.6μs | 7742μs | 65591B |
| 1000×100B | ext-grpc | 5095.2μs | 9038.2μs | 188950.6 | 7768.3μs | - | - |
| 10×100KiB | libcurl | 917.4μs | 3639.9μs | 8318.2 | 3016.1μs | - | - |
| 10×100KiB | MVP direct | 480.0μs | 3465.0μs | 12219.2 | 3277.1μs | 3299μs | 0B |
| 10×100KiB | MVP compact64 | 506.0μs | 3340.0μs | 12620.3 | 3227.2μs | 3205μs | 102409B |
| 10×100KiB | ext-grpc | 633.9μs | 3379.3μs | 10965.5 | 2874.9μs | - | - |
| 100×100KiB | libcurl | 5062.3μs | 10341.3μs | 18210.2 | 9837.8μs | - | - |
| 100×100KiB | MVP direct | 5473.0μs | 12747.0μs | 16702.3 | 12611.1μs | 10865μs | 0B |
| 100×100KiB | MVP compact64 | 5393.0μs | 12396.0μs | 16773.6 | 12264.4μs | 10777μs | 102409B |
| 100×100KiB | ext-grpc | 5407.4μs | 10760.4μs | 17523.1 | 10631.9μs | - | - |
| 1×1MiB | libcurl | 899.0μs | 5406.4μs | 792.3 | 4484.3μs | - | - |
| 1×1MiB | MVP direct | 486.0μs | 3985.0μs | 1281.0 | 3675.4μs | 3835μs | 0B |
| 1×1MiB | MVP compact64 | 491.0μs | 4109.0μs | 1316.6 | 3582.6μs | 3801μs | 1048585B |
| 1×1MiB | ext-grpc | 527.7μs | 3852.0μs | 1277.2 | 3250.2μs | - | - |
| 10000×100B | libcurl | 43996.8μs | 56445.6μs | 224598.7 | 55852.4μs | - | - |
| 10000×100B | MVP direct | 42052.0μs | 50780.0μs | 233831.1 | 50659.6μs | 45354μs | 0B |
| 10000×100B | MVP compact64 | 41925.0μs | 53243.0μs | 235003.4 | 52900.7μs | 47303μs | 65591B |
| 10000×100B | ext-grpc | 43531.5μs | 57483.6μs | 226376.7 | 56125.7μs | - | - |

## server streaming 判断

MVPは多くのstreaming shapeでext-grpcと同等以上にいる。

- `1000×100B`: MVP direct/compact64ともext-grpcよりp50/p99/msg/sが良い。
- `10×100KiB`: MVP compact64はext-grpcよりp99がわずかに良く、msg/sも高い。
- `1×1MiB`: MVP directはext-grpcとほぼ同等。p50とmsg/sはMVPが良く、p99はext-grpcが約133μs良い。
- `10000×100B`: MVP direct/compact64ともext-grpcより良い。
- `100×100KiB`: このrunではlibcurlが最良、ext-grpcが次点、MVPは悪い。server last p99も同時に悪化しているため、client内CPUだけの問題ではなくreceive progression / server send progressの揺れを含む。

`100×100KiB` は前回 `poc-goal` 比較ではMVP compact64がext-grpc同等だったため、run間揺れが大きい。最終判断前にrepeatまたはdecision runで再取得する価値がある。

## 総合判断

native transport MVPはPhase 2の実装方向として十分成立する。

- large request unaryではlibcurlを明確に上回り、ext-grpcと同等レンジ。
- server streaming large responseでは多くの代表形状でext-grpc同等以上。
- response pathは `direct payload assembly` と `compact/ring buffer` の両方が必要。
- libcurl継続は本実装の主経路から外す判断でよい。

ただしMVPのproduction化には以下が残る。

- `nghttp2_poc_unary_batch()` は計測用APIであり、production API/lifetimeへ作り直す必要がある。
- current opt-in PHP wrapperはsmoke用で、connection/session lifetimeをまだChannelに持たない。
- TLS/mTLS/deadline/metadata/status validation/error semanticsはlibcurl実装同等に移植が必要。
- server streamingのactual `responses()` incremental delivery surfaceで再計測が必要。
