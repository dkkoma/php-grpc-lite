# nghttp2 PoC: bounded read-ahead for server streaming (2026-05-03)

## 目的

前回のread-ahead PoCでは、complete response payloadをqueueへ積み、socket drain後にまとめてPHP callback/decode/yieldへ渡した。`1×1MiB` では少し効く一方、many-small / long streamではqueue waitとdelivery latencyが大きくなった。

今回は無制限read-aheadではなく、message数またはbyte数で上限を持つbounded read-aheadを試した。

## 実装

`nghttp2_poc_unary_batch()` に以下を追加した。

- `read_ahead_max_messages`
- `read_ahead_max_bytes`

`bench.php` のCLI option:

- `--read-ahead-max-messages=N`
- `--read-ahead-max-bytes=N`

挙動:

1. `--direct-response-payload --read-ahead-delivery` のときだけ有効。
2. complete payloadをqueueへ積む。
3. queueのmessage数またはbytesが上限以上になったら、その時点でqueueをdeliverする。
4. 上限未指定なら従来通り、receive drain後にdeliverする。

## 再実行方法

```bash
BENCH_TAG=20260503-bounded-read-ahead bench/compare-server-stream-bounded-read-ahead.sh
```

出力:

- summary: `var/bench-results/phase2-server-stream-bounded-read-ahead-20260503-bounded-read-ahead.tsv`
- per-run JSON: `var/bench-results/phase2-server-stream-bounded-read-ahead-20260503-bounded-read-ahead-*.json`

固定条件:

- RPC: `BenchServerStream`
- PoC共通: `--poll-loop --no-copy --flush-after-mem-recv --incremental-decode --response-callback-mode=decode-yield --direct-response-payload`
- 候補: direct, unbounded read-ahead, max messages 1/4/16, max bytes 64KiB/256KiB/1MiB

## best候補

p99が最も良い候補をcaseごとに選ぶと以下。

| case | best | p50 | p99 | direct p99 | p99 delta vs direct | msg/s | server last p99 | client residual p99 | poll wait p99 | queue count p99 | queue wait p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1000×100B | direct | 4592.0μs | 8508.0μs | 8508.0μs | 0.0μs | 207562.0 | 7880.1μs | 627.9μs | 8054μs | 0 | 7μs |
| 10×100KiB | direct | 478.0μs | 3026.0μs | 3026.0μs | 0.0μs | 12278.0 | 2809.5μs | 216.5μs | 2836μs | 0 | 1μs |
| 100×100KiB | read_ahead_256k | 5649.0μs | 10128.0μs | 11213.0μs | -1085.0μs | 16913.6 | 9884.4μs | 243.6μs | 8883μs | 3 | 353μs |
| 1×1MiB | read_ahead_msg16 | 463.0μs | 3616.0μs | 4121.0μs | -505.0μs | 1350.7 | 3129.9μs | 486.1μs | 3487μs | 1 | 10μs |
| 10000×100B | read_ahead_msg4 | 43367.0μs | 51503.0μs | 58773.0μs | -7270.0μs | 230011.0 | 51422.2μs | 80.8μs | 45806μs | 4 | 2498μs |

## 全体傾向

bounded read-aheadは万能ではない。

- `1000×100B` と `10×100KiB` はdirectが最良。read-aheadを入れる理由は弱い。
- `100×100KiB` は `read_ahead_256k` がp99を約1.1ms改善した。
- `1×1MiB` はmessage数上限系が効き、今回runでは `read_ahead_msg16` が最良だった。ただしmessage数は1なので、効果は「上限」そのものよりrun間のserver tail揺れも混ざる。
- `10000×100B` は `read_ahead_msg4` がp99を約7.3ms改善したが、server last p99も同時に下がっている。client residual p99は80.8μsで、client側queue改善だけで説明するべきではない。

## 判断

bounded read-aheadをHTTP/2 transportの標準経路に入れる根拠はまだ弱い。

採用するなら以下の限定条件にする。

1. direct payload assembly path上のoptional knobにする。
2. defaultはread-aheadなし。
3. large/long streamのtailを見て、bytes上限つきで有効化する余地を残す。
4. many-small streamでmessage delivery latencyを増やす設定は避ける。

今回の主結論は、read-aheadよりも引き続き以下が本筋ということ。

- append-only body bufferを捨てる。
- compact/ring bufferとdirect payload assemblyをshape別に使う。
- receive loop / window / buffer tuningはtransport側で保持する。

## 次に見ること

read-ahead単体ではext-grpcとの差を決定的には説明しない。次は、call単位ではなくchannel単位でHTTP/2 session / event loopを所有するPoCを試す。

狙い:

- 複数callを同一connection / session上で進める。
- callごとのblocking loopではなく、channel transportが全streamをまとめてpoll/recv/sendする。
- ext-grpcのC-core channel / completion queueに近い構造の効果を、採用判断とは別に観測する。
