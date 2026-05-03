# nghttp2 PoC: server streaming poll wait causality (2026-05-03)

## 目的

前回のp99残差分解で、残るwall timeの大半は `poll wait` として見えた。ただし `poll wait` は原因ではなく、socket readinessを待っている観測上の場所でしかない。

今回は、`poll wait` が以下のどれに近いかを切る。

1. poll復帰後にDATA callbackまで時間がかかっている。
2. read loopがsocketをdrainし切れておらず、余分にpollへ戻っている。
3. WINDOW_UPDATE送信後に次DATAが来るまで待っている。
4. read-firstのloop順序で改善する。
5. 単にsocket readableになるまで待っている。

## 追加した計測

`nghttp2_poc` に以下を追加した。

- `call_poll_to_data_us`
- `call_max_poll_to_data_us`
- `call_window_update_to_data_us`
- `call_max_window_update_to_data_us`
- `call_receive_drains`
- `call_receive_drains_with_data`
- `call_receive_drains_eagain_after_data`
- `call_max_reads_per_drain`
- `call_max_bytes_per_drain`

`receive_available()` は1回のdrainで `recv()` が正のbyteを返した回数とbyte数を記録し、最後に `EAGAIN` へ到達したらdrain完了として数える。これにより、poll前にsocketを読み切っているかを見られる。

## 結果

固定条件:

- `server-stream`
- `decode-yield`
- `--poll-loop`
- `--no-copy`
- `--flush-after-mem-recv`
- `--incremental-decode`

| case | path | p50 | p99 | poll wait p99 | poll→DATA p99 | max poll→DATA p99 | WU→DATA p99 | max WU→DATA p99 | drains p99 | drains with data p99 | max reads/drain p99 | max bytes/drain p99 | recv calls p99 | DATA callbacks p99 | WU sent p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1×1MiB | compact | 523.0μs | 4177.0μs | 3993.0μs | 68.0μs | 20.0μs | 0.0μs | 0.0μs | 35 | 34 | 17 | 1049302B | 69 | 97 | 1 |
| 1×1MiB | direct | 449.0μs | 3878.0μs | 3799.0μs | 69.0μs | 27.0μs | 0.0μs | 0.0μs | 35 | 34 | 17 | 1049302B | 69 | 97 | 1 |
| 100×100KiB | compact | 5484.0μs | 11224.0μs | 9847.0μs | 654.0μs | 299.0μs | 1373.0μs | 1365.0μs | 339 | 338 | 115 | 7471517B | 704 | 980 | 5 |
| 100×100KiB | direct | 5365.0μs | 11587.0μs | 10405.0μs | 766.0μs | 369.0μs | 1048.0μs | 1022.0μs | 346 | 345 | 91 | 5943417B | 712 | 982 | 5 |

## tail call確認

### 1×1MiB direct

```text
lat=7007us poll=6859us pollToData=53us maxPollToData=3us
wuToData=0us drains=35 drainsData=34 readsMax=1 bytesMax=32768
recvCalls=69 dataCb=97 dataBytes=1048585 wuSent=0
close=7007 lastData=6987 cbDone=7006
```

別tailでは1回のdrainで大きく読めている。

```text
lat=5675us poll=5565us pollToData=31us maxPollToData=5us
drains=16 drainsData=15 readsMax=10 bytesMax=622592
recvCalls=40 dataCb=87 dataBytes=1048585
```

### 100×100KiB compact

```text
lat=12920us poll=11489us pollToData=415us maxPollToData=11us
wuToData=14us maxWuToData=5us drains=179 drainsData=178
readsMax=25 bytesMax=1606784 recvCalls=472 dataCb=913 wuSent=4
```

別tailではWINDOW_UPDATE後の次DATA待ちが見える。

```text
lat=11402us poll=10106us pollToData=271us maxPollToData=7us
wuToData=318us maxWuToData=308us drains=106 drainsData=105
readsMax=26 bytesMax=1643720 recvCalls=348 dataCb=885 wuSent=4
```

## read-first比較

`--read-first-poll-loop` も確認した。

| case | path | read-first | p50 | p99 | poll wait p99 | poll→DATA p99 | drains p99 | max reads/drain p99 | max bytes/drain p99 | recv calls p99 | DATA callbacks p99 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1×1MiB | direct | no | 486.0μs | 3938.0μs | 3794.0μs | 78.0μs | 35 | 17 | 1049303B | 69 | 97 |
| 1×1MiB | direct | yes | 465.0μs | 4093.0μs | 3955.0μs | 68.0μs | 69 | 17 | 1049302B | 103 | 97 |
| 100×100KiB | compact | no | 5267.0μs | 11482.0μs | 10186.0μs | 625.0μs | 304 | 97 | 6222192B | 652 | 965 |
| 100×100KiB | compact | yes | 5229.0μs | 11113.0μs | 9635.0μs | 582.0μs | 311 | 104 | 6763152B | 656 | 967 |

read-firstは決定的な改善ではない。100×100KiBでは少し良いが、1×1MiBではむしろp99が悪化した。

## 判断

今回の計測から言えることは以下。

1. poll復帰後にDATA callbackへ到達する時間は小さい。`poll_to_data` はp99でも1×1MiBで約70μs、100×100KiBで約650〜760μsで、全体のpoll wait p99を説明する主因ではない。
2. read loopは基本的にEAGAINまでdrainしている。各tailで `drains_with_data` が多く、1回のdrainで数百KiB〜数MiBを読めているケースもある。
3. 1×1MiBではWINDOW_UPDATE後DATA待ちはほぼ出ない。receive windowが十分で、flow-control待ちではない。
4. 100×100KiBではWINDOW_UPDATE後DATA待ちが一部見える。ただし全体のpoll wait p99全部を説明するほどではなく、tailによってばらつく。
5. read-firstで構造的に解消する問題ではない。

## 結論

`poll wait` はPoC側の「poll復帰後処理が遅い」「socketを読み切れていない」「WINDOW_UPDATE送信が常に遅い」という形のボトルネックではなかった。

残る差分は、より正確には以下として扱う。

- socketがreadableになるまでの待ち。
- server/gRPC library/HTTP2 stackがDATAを実際にsocketへ流す粒度。
- OS schedulerとDocker network上のreadiness通知の揺れ。
- 100×100KiBでは一部flow-control/WINDOW_UPDATE後DATA待ちも混ざる。

ext-grpcとの差がここに出るなら、ext-grpcが「pollしない」のではなく、C-coreのevent engine、HTTP/2 read/write scheduling、WINDOW_UPDATE発行、serverとの相互作用によって、socket readableになるタイミングやDATA到着粒度が異なる可能性が高い。

PoC側で確認できる範囲では、read loopはEAGAINまでdrainしており、poll後の処理遅延も小さいため、クライアントuserland処理の未説明ボトルネックとして残っているわけではない。
