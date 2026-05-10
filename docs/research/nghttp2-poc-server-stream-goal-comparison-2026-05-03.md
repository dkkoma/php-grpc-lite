# nghttp2 PoC: server streaming goal candidate comparison (2026-05-03)

## 目的

server streaming large response pathのPoCゴール候補として、以下がext-grpcに対してどの程度の位置にいるかを確認した。

- compact/ring buffer相当: `--compact-response-buffer --response-compact-threshold=65536`
- direct payload assembly: `--direct-response-payload`

現PoCでは `direct-response-payload` と `compact-response-buffer` は同時には効かない。direct pathはgRPC frame headerを読んだ時点で最終payload stringへDATAを組み立てるため、body buffer自体を使わない。compact pathは汎用body bufferを使う場合にconsumed bytesを捨てる経路である。

したがって、この比較での「ゴール候補」は単一flagではなく、call shapeに応じて以下を選ぶ実装方針を指す。

1. long/many-message streamではcompact/ring bufferでbody bufferを有界に保つ。
2. large single/large payloadではdirect payload assemblyで中間body bufferと二重copyを避ける。

## 再実行方法

比較入口を追加した。

```bash
BENCH_TAG=20260503-poc-goal-server-stream bench/compare-server-stream-poc-goal.sh
```

出力:

- summary: `var/bench-results/phase2-server-stream-poc-goal-20260503-poc-goal-server-stream.tsv`
- per-run JSON: `var/bench-results/phase2-server-stream-poc-goal-20260503-poc-goal-server-stream-*.json`

固定条件:

- RPC: `BenchServerStream`
- decode/yield込み
- PoC共通: `--poll-loop --no-copy --flush-after-mem-recv --incremental-decode --response-callback-mode=decode-yield`
- ext-grpc / php-grpc-lite: `tools/benchmark/streaming-diagnostic.php`

## 全パターン比較

| case | implementation | p50 | p99 | msg/s | server last p99 | client residual p99 | poll wait p99 | max body buffer p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1000×100B | php-grpc-lite | 4792.2μs | 8198.0μs | 199710.3 | 7707.4μs | 490.6μs | - | - |
| 1000×100B | ext-grpc | 5093.0μs | 9270.1μs | 189817.8 | 7283.1μs | 1986.9μs | - | - |
| 1000×100B | poc-direct | 4481.0μs | 8373.0μs | 215130.1 | 8320.1μs | 52.9μs | 7936μs | 0B |
| 1000×100B | poc-compact64 | 4477.0μs | 7662.0μs | 215727.7 | 7569.3μs | 92.7μs | 7187μs | 65591B |
| 10×100KiB | php-grpc-lite | 935.3μs | 3983.2μs | 8275.7 | 3534.6μs | 448.6μs | - | - |
| 10×100KiB | ext-grpc | 644.2μs | 3490.0μs | 10937.5 | 3341.9μs | 148.0μs | - | - |
| 10×100KiB | poc-direct | 495.0μs | 3512.0μs | 11859.6 | 3407.1μs | 104.9μs | 3377μs | 0B |
| 10×100KiB | poc-compact64 | 552.0μs | 3825.0μs | 11796.5 | 3453.6μs | 371.4μs | 3397μs | 102409B |
| 100×100KiB | php-grpc-lite | 5370.7μs | 14405.3μs | 16984.2 | 13905.6μs | 499.8μs | - | - |
| 100×100KiB | ext-grpc | 5836.5μs | 11490.1μs | 16235.0 | 11145.6μs | 344.5μs | - | - |
| 100×100KiB | poc-direct | 5780.0μs | 12827.0μs | 16110.8 | 12351.8μs | 475.2μs | 11478μs | 0B |
| 100×100KiB | poc-compact64 | 5587.0μs | 11381.0μs | 16614.0 | 10530.0μs | 851.0μs | 9887μs | 102409B |
| 1×1MiB | php-grpc-lite | 986.9μs | 4767.1μs | 743.9 | 4002.9μs | 764.2μs | - | - |
| 1×1MiB | ext-grpc | 558.8μs | 4067.7μs | 1236.4 | 3324.8μs | 742.9μs | - | - |
| 1×1MiB | poc-direct | 492.0μs | 3806.0μs | 1331.5 | 3265.3μs | 540.7μs | 3683μs | 0B |
| 1×1MiB | poc-compact64 | 536.0μs | 4075.0μs | 1281.7 | 3446.6μs | 628.4μs | 3958μs | 1048585B |
| 10000×100B | php-grpc-lite | 45353.7μs | 85176.6μs | 213790.2 | 84860.1μs | 316.5μs | - | - |
| 10000×100B | ext-grpc | 42597.6μs | 49197.2μs | 232950.6 | 48618.2μs | 579.0μs | - | - |
| 10000×100B | poc-direct | 44656.0μs | 56757.0μs | 221628.5 | 56084.2μs | 672.8μs | 51785μs | 0B |
| 10000×100B | poc-compact64 | 44851.0μs | 61925.0μs | 218487.3 | 61137.8μs | 787.2μs | 55551μs | 65591B |

## ext-grpc比

p99が最も良いPoC候補をcaseごとに選ぶと以下。

| case | best PoC | PoC p50 / ext p50 | PoC p99 / ext p99 | PoC msg/s / ext msg/s |
| --- | --- | ---: | ---: | ---: |
| 1000×100B | poc-compact64 | 0.88 | 0.83 | 1.14 |
| 10×100KiB | poc-direct | 0.77 | 1.01 | 1.08 |
| 100×100KiB | poc-compact64 | 0.96 | 0.99 | 1.02 |
| 1×1MiB | poc-direct | 0.88 | 0.94 | 1.08 |
| 10000×100B | poc-direct | 1.05 | 1.15 | 0.95 |

## 判断

`compact/ring buffer + direct payload assembly` は、server streaming large responseのPoCゴール候補として十分な位置にある。

- `1000×100B`, `100×100KiB`, `1×1MiB` ではext-grpcのp99を下回った。
- `10×100KiB` はp99がほぼ同等で、p50とmsg/sはPoCが良い。
- `10000×100B` はext-grpcよりp99が悪いが、client residual p99差は約94μsで、wall差の大半は別runのserver last p99差として出ている。ここだけでclient側の未解決ボトルネックとは判断しない。

path選択としては以下が妥当。

1. many-small / long stream: compact/ring bufferを基本にする。
2. 1messageが大きいstream: direct payload assemblyを基本にする。
3. どちらのpathでもappend-only body bufferは採用しない。

## C拡張実装への含意

本実装で狙うべきresponse pathは以下。

- HTTP/2 DATA受信はnghttp2を直接駆動し、libcurlのresponse body集約には戻さない。
- gRPC frame parserはC側に置き、message境界をDATA受信中に確定する。
- 汎用受信bufferを使う場合はring bufferまたはcompact bufferでconsumed bytesを保持し続けない。
- large payloadでは最終payload bufferへ直接組み立て、`DATA -> body -> payload` の二重copyを避ける。
- PHP userlandへ渡す単位はext-grpc互換のmessage object/yieldに合わせるが、transport側のbuffer lifetimeはmessage delivery後に即解放する。

## 残る確認

`10000×100B` はrun間のserver tail差が大きい。client側をさらに説明するなら、同一process内でext-grpcとPoCを交互に回すことはできないため、repeatを増やしてserver last p99を揃えた比較にする必要がある。
