# nghttp2 PoC: server streaming p99 residual breakdown (2026-05-03)

## 目的

server streaming の p99 差分について、改善候補の有無とは別に、どこに wall time があるかを同一call内で説明できる状態にする。

これまでの `p99 - server last p99` は、別々の分布のp99同士を引いていたため、tail callそのものの説明としては弱かった。今回は以下の境界を追加し、同一call内の残差を出した。

- last response DATA callback time
- complete gRPC message ready time
- PHP callback done time
- stream close time
- loop top-level active time
- loop known time including poll wait
- unaccounted time after loop known time

## 実装した計測

`nghttp2_poc` に以下を追加した。

- `client_first_response_message_ready_us`
- `client_last_response_message_ready_us`
- `client_first_response_callback_done_us`
- `client_last_response_callback_done_us`

`bench.php` にはderived seriesを追加した。

- `call_loop_active_us`: `recv syscall + nghttp2_session_mem_recv + nghttp2_session_send(after recv)`
- `call_loop_known_including_poll_us`: `call_loop_active_us + poll wait`
- `call_unaccounted_after_loop_known_us`: `latency - call_loop_known_including_poll_us`
- `call_mem_recv_inner_known_us`: `body append/compact + payload build + callback`
- `call_after_last_data_to_close_us`
- `call_after_message_ready_to_close_us`
- `call_after_callback_done_to_close_us`
- `call_server_last_to_close_us`

注意点として、`call_mem_recv_inner_known_us` は `nghttp2_session_mem_recv()` の内側で発生するcallback系時間なので、loop-level partitionへ足してはいけない。p99差分のpartitionは `poll wait + recv syscall + mem_recv + session_send + unaccounted` で見る。

## 結果

固定条件:

- `server-stream`
- `decode-yield`
- `--poll-loop`
- `--no-copy`
- `--flush-after-mem-recv`
- `--incremental-decode`

| case | path | p50 | p99 | server last p99 | server→close p99 | last DATA→close p99 | ready→close p99 | callback done→close p99 | loop active p99 | poll wait p99 | loop known+poll p99 | unaccounted p99 | mem_recv inner p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1×1MiB | append | 511.0μs | 4376.0μs | 3893.3μs | 1605.0μs | 119.0μs | 119.0μs | 16.0μs | 473.0μs | 4226.0μs | 4374.0μs | 7.0μs | 136.0μs |
| 1×1MiB | compact | 489.0μs | 3705.0μs | 3338.1μs | 1934.0μs | 88.0μs | 88.0μs | 10.0μs | 414.0μs | 3545.0μs | 3705.0μs | 6.0μs | 112.0μs |
| 1×1MiB | direct | 479.0μs | 4037.0μs | 3519.4μs | 1879.0μs | 64.0μs | 64.0μs | 17.0μs | 308.0μs | 3787.0μs | 4036.0μs | 7.0μs | 86.0μs |
| 100×100KiB | append | 5221.0μs | 11721.0μs | 11404.6μs | 1461.0μs | 13.0μs | 12.0μs | 4.0μs | 4325.0μs | 8427.0μs | 11463.0μs | 252.0μs | 3150.0μs |
| 100×100KiB | compact | 5285.0μs | 10522.0μs | 10455.3μs | 1093.0μs | 47.0μs | 46.0μs | 41.0μs | 2017.0μs | 9107.0μs | 10507.0μs | 36.0μs | 1048.0μs |
| 100×100KiB | direct | 5699.0μs | 12017.0μs | 11937.1μs | 1284.0μs | 12.0μs | 12.0μs | 6.0μs | 1889.0μs | 10721.0μs | 11987.0μs | 34.0μs | 1026.0μs |

## tail call確認

p99近辺の同一callを見ると、未説明時間はほぼ残っていない。

`1×1MiB direct` のtail例:

```text
lat=7022us close=7020 lastData=6999 ready=6999 cbDone=7018
active=128us poll=6938us known=7066us unaccounted=0us
recv=23us mem=52us payload=26us callback=19us append=0us
```

`100×100KiB compact` のtail例:

```text
lat=12991us close=12991 lastData=12986 ready=12986 cbDone=12990
active=2486us poll=11378us known=13864us unaccounted=0us
recv=604us mem=964us payload=189us callback=490us append=211us
```

ここで `known > latency` になることがあるのは、`mem_recv` がcallback系処理を内包しており、旧式のactive集計では内側時間を二重計上していたため。loop-levelでは `poll + recv + mem_recv + send_after_recv` をpartitionとして扱う。

## 判断

p99のwall timeは、loop-levelではほぼ説明できる。

- stream close直前の処理、つまり last DATA到着後、message ready後、callback done後の処理は小さい。
- `1×1MiB` の tail は大半が `poll wait`、次に `mem_recv/recv/session_send`。
- `100×100KiB` の append pathでは `mem_recv` 内側のappend/callbackが大きく、compact/directでここは下がる。
- compact/direct後のlong streamでは、残るp99は主にpoll wait、つまり「次のDATAが来るまで待っている時間」として見える。
- `unaccounted` はp99で数十μs程度まで落ちており、クライアント内で未観測の大きな処理塊は残っていない。

## 結論

改善候補の観点では、以下でほぼ切れている。

1. append-only response bufferはlong streamで明確に悪い。
2. compact/ring bufferでlong streamのappend/mem_recv内側コストは下げられる。
3. large single messageではdirect payload assemblyでbody buffer二重copyを避けられる。
4. protobuf decode/yield単体は主要因ではない。

残るp99差分は、PoCの観測範囲では主に `poll wait` として説明される。これはクライアントCPU内の未観測ボトルネックというより、server/transport pacing、socket readiness、HTTP/2 DATA到着粒度を待っている時間である。

次にさらに詰める場合は、改善実装ではなく、serverとclientのclock対応をより厳密に取る、または同一プロセス内/固定scheduler条件でtransport pacingの揺れを取り除く計測になる。現時点では、C拡張実装に向けたクライアント側の大きな未説明ボトルネックは見えていない。
