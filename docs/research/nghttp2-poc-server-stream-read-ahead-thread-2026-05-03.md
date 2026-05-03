# nghttp2 PoC: server streaming read-ahead and transport thread (2026-05-03)

## 目的

ext-grpcとの差分仮説として、C-coreがPHP main thread外でtransportを進め、messageを先読みしている可能性がある。採用判断は別として、以下をPoCで検証した。

1. single-thread read-aheadだけで差が縮むか。
2. transport専用threadで差が縮むか。
3. どのcall shapeで有効/悪化するか。

## 実装

### read-ahead delivery

`--read-ahead-delivery` を追加した。

- `--direct-response-payload` と組み合わせて使う。
- DATAを最終payload bufferへ組み立てる。
- complete messageになっても即PHP callbackへ渡さない。
- `receive_available()` がsocketをEAGAINまでdrainした後、queued payloadをmain threadでcallback/decode/yieldする。

追加計測:

- `call_max_response_queue_count`
- `call_max_response_queue_bytes`
- `call_response_queue_wait_us`
- `call_max_response_queue_wait_us`

### transport thread

`--transport-thread` を追加した。

- transport threadが `drive_stream_poll()` を実行する。
- transport thread内ではZend APIを触らない。
- DATA payloadは `malloc()` bufferへ組み立て、raw payload queueへ積む。
- main threadは `pthread_join()` 後、raw payloadをPHP string化してcallback/decode/yieldする。

これは本実装候補ではなく、transportをmain thread外で進めた場合にp99構造が変わるかを見るためのPoCである。実運用のserver streamingとは異なり、messageを逐次アプリへ返すのではなく、call完了後にまとめてcallbackへ渡す。

## 初回比較

固定条件:

- `server-stream`
- `decode-yield`
- `--poll-loop`
- `--no-copy`
- `--flush-after-mem-recv`
- `--incremental-decode`
- `--direct-response-payload`

| case | path | p50 | p99 | msg/s | poll wait p99 | queue count p99 | queue bytes p99 | queue wait p99 | callback p99 | server last p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1×1MiB | direct | 556.0μs | 4551.0μs | 1207.7 | 4338.0μs | 0 | 0B | 1.0μs | 54.0μs | 3772.2μs |
| 1×1MiB | read_ahead | 522.0μs | 4149.0μs | 1269.9 | 4002.0μs | 1 | 1048580B | 10.0μs | 35.0μs | 3411.2μs |
| 1×1MiB | thread | 581.0μs | 4109.0μs | 1188.5 | 3861.0μs | 1 | 1048580B | 200.0μs | 48.0μs | 3764.6μs |
| 100×100KiB | direct | 5938.0μs | 12267.0μs | 16042.9 | 10909.0μs | 0 | 0B | 6.0μs | 603.0μs | 12165.1μs |
| 100×100KiB | read_ahead | 6033.0μs | 12221.0μs | 15830.8 | 10572.0μs | 65 | 6656260B | 43561.0μs | 598.0μs | 12073.5μs |
| 100×100KiB | thread | 5867.0μs | 12155.0μs | 15926.5 | 10048.0μs | 100 | 10240400B | 631247.0μs | 478.0μs | 11404.8μs |
| 1000×100B | direct | 4771.0μs | 17173.0μs | 193800.1 | 10430.0μs | 0 | 0B | 29.0μs | 1173.0μs | 16588.4μs |
| 1000×100B | read_ahead | 4743.0μs | 9914.0μs | 199834.1 | 8486.0μs | 665 | 67830B | 169284.0μs | 1257.0μs | 9668.1μs |
| 1000×100B | thread | 5151.0μs | 7808.0μs | 189243.5 | 7292.0μs | 1000 | 102000B | 3370876.0μs | 376.0μs | 7313.1μs |

## repeat確認

初回は `1000×100B direct` のtailが大きく荒れたため、`1×1MiB` と `1000×100B` を再取得した。

| case | path | p50 | p99 | msg/s | poll wait p99 | queue count p99 | queue bytes p99 | queue wait p99 | callback p99 | server last p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1×1MiB | direct | 567.0μs | 4167.0μs | 1202.5 | 3901.0μs | 0 | 0B | 1.0μs | 54.0μs | 3559.9μs |
| 1×1MiB | read_ahead | 558.0μs | 4113.0μs | 1239.8 | 3920.0μs | 1 | 1048580B | 13.0μs | 48.0μs | 3761.6μs |
| 1×1MiB | thread | 604.0μs | 4277.0μs | 1167.7 | 4071.0μs | 1 | 1048580B | 84.0μs | 52.0μs | 3429.1μs |
| 1000×100B | direct | 4898.0μs | 9296.0μs | 192433.6 | 8592.0μs | 0 | 0B | 36.0μs | 1033.0μs | 9088.7μs |
| 1000×100B | read_ahead | 4925.0μs | 8956.0μs | 197087.7 | 8368.0μs | 521 | 53142B | 143357.0μs | 691.0μs | 8825.7μs |
| 1000×100B | thread | 5411.0μs | 9280.0μs | 176667.7 | 8680.0μs | 1000 | 102000B | 4180951.0μs | 483.0μs | 8764.0μs |

## 判断

### read-ahead

`1×1MiB` では初回 p99 が `4551us -> 4149us` まで縮み、repeatでも `4167us -> 4113us` とわずかに縮んだ。large single messageでは、callback/decodeをtransport drain後へずらす効果が少しある。

一方、`100×100KiB` と `1000×100B` ではqueueが大きくなる。特にmany-smallではqueue waitが大きく、streaming APIとしての逐次delivery latencyを悪化させる。read-aheadを無制限に入れるのはよくない。

### transport thread

transport threadは、初回の `1000×100B` では p99 が `17173us -> 7808us` と良く見えたが、repeatでは `9296us -> 9280us` でほぼ差がない。さらにp50とthroughputは悪化した。

`1×1MiB` ではrepeatで direct/read-ahead より悪く、thread起動/joinとraw queue deliveryのコストが見える。`100×100KiB` でもp99はほぼ横ばいで、queue waitは大きい。

このPoCでは、transport thread化そのものが安定した改善策とは言えない。

## 結論

ext-grpcとの差分仮説として「main thread外でtransportを進めると有利」は検証したが、今回のPoCでは決定的な改善は出なかった。

- `1×1MiB`: read-aheadはわずかに効くが、threadは安定して勝たない。
- `100×100KiB`: read-ahead/threadともqueueが大きくなり、wall p99はほぼ横ばい。
- `1000×100B`: threadは一部runでp99改善に見えたがrepeatでは再現せず、throughputが落ちる。

採用候補としては、無制限read-aheadやcallごとのthread起動ではなく、以下が現実的。

1. server streamingは逐次deliveryを基本にする。
2. long streamではcompact/ring bufferを使う。
3. large single messageではdirect payload assemblyを使う。
4. read-aheadを入れるとしても、message数/byte数に上限を持つ bounded read-ahead に限定する。
5. 専用threadは、少なくともこのPoC結果だけでは採用理由にならない。

残るext-grpcとの差を説明するなら、単に「別threadだから速い」ではなく、C-coreの永続event engine、pollset、複数call共有のtransport、queue/backpressure、server側HTTP/2 stackとの相互作用まで含めた構造差として見るべき。
